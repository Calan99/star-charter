// ephemeris.c
// 
// -------------------------------------------------
// Copyright 2015-2022 Dominic Ford
//
// This file is part of StarCharter.
//
// StarCharter is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// StarCharter is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with StarCharter.  If not, see <http://www.gnu.org/licenses/>.
// -------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "astroGraphics/ephemeris.h"
#include "coreUtils/asciiDouble.h"
#include "coreUtils/errorReport.h"
#include "mathsTools/julianDate.h"
#include "mathsTools/projection.h"
#include "mathsTools/sphericalTrig.h"
#include "settings/chart_config.h"
#include "vectorGraphics/lineDraw.h"
#include "vectorGraphics/cairo_page.h"

//! ephemerides_fetch - Fetch the ephemeris data for solar system objects to be plotted on a star chart
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.

void ephemerides_fetch(chart_config *s) {
    int i, j, k;
    int total_ephemeris_points = 0;

    // Track the sky coverage of each ephemeris in RA and Dec
    // Create a coarse grid of RA and Declination where we set Boolean flags for whether the solar system object passes
    // through each cell.
    int ra_bins = 24 * 8;
    int dec_bins = 18 * 8;
    int *ra_usage = (int *) malloc(ra_bins * sizeof(int));
    int *dec_usage = (int *) malloc(dec_bins * sizeof(int));

    // Zero map of sky coverage
    for (i = 0; i < ra_bins; i++) ra_usage[i] = 0;
    for (i = 0; i < dec_bins; i++) dec_usage[i] = 0;

    // Allocate storage for the ephemeris of each solar system object
    s->ephemeris_data = (ephemeris *) malloc(s->ephmeride_count * sizeof(ephemeris));

    // Loop over each of the solar system objects we are plotting tracks for
    for (i = 0; i < s->ephmeride_count; i++) {
        // Fetch the string definition, passed by the user
        // For example: jupiter,2458849.5,2459216.5
        const char *trace_definition = s->ephemeris_definitions[i];

        // Extract object name, jd_min and jd_max from trace definition string
        const char *in_scan = trace_definition;
        char object_id[FNAME_LENGTH], buffer[FNAME_LENGTH];
        // Read object name into <object_id>
        str_comma_separated_list_scan(&in_scan, object_id);
        // Read starting Julian day number into s->ephemeris_data[i].jd_start
        str_comma_separated_list_scan(&in_scan, buffer);
        s->ephemeris_data[i].jd_start = get_float(buffer, NULL);
        // Read ending Julian day number into s->ephemeris_data[i].jd_end
        str_comma_separated_list_scan(&in_scan, buffer);
        s->ephemeris_data[i].jd_end = get_float(buffer, NULL);
        // Sample planet's movement every 12 days
        s->ephemeris_data[i].jd_step = 0.5;

        // Generous estimate of how many lines we expect ephemerisCompute to return
        s->ephemeris_data[i].point_count = (int) (20 +
                                                  (s->ephemeris_data[i].jd_end - s->ephemeris_data[i].jd_start) /
                                                  s->ephemeris_data[i].jd_step
        );

        // Allocate data to hold the ephemeris
        s->ephemeris_data[i].data = (ephemeris_point *) malloc(
                s->ephemeris_data[i].point_count * sizeof(ephemeris_point)
        );

        // Use ephemeris-compute-de430 to track the path of this object
        char ephemeris_compute_command[FNAME_LENGTH];

        // Construct a command-line to run the ephemeris generation tool
        snprintf(ephemeris_compute_command, FNAME_LENGTH, "%.2048s "
                                                          "--jd_min %.15f "
                                                          "--jd_max %.15f "
                                                          "--jd_step %.15f "
                                                          "--output_format 1 "
                                                          "--output_constellations 0 "
                                                          "--output_binary 0 "
                                                          "--objects \"%.256s\" ",
                 s->ephemeris_compute_path,
                 s->ephemeris_data[i].jd_start, s->ephemeris_data[i].jd_end, s->ephemeris_data[i].jd_step, object_id);

        // Run ephemeris generator
        FILE *ephemeris_data = popen(ephemeris_compute_command, "r");
        // printf("%s\n", ephemeris_compute_command);

        // Loop over the lines returned by ephemeris-compute-de430
        int line_counter = 0;
        const char *previous_label = "";
        while ((!feof(ephemeris_data)) && (!ferror(ephemeris_data))) {
            char line[FNAME_LENGTH], label[FNAME_LENGTH], *scan;

            // Read line of output text
            file_readline(ephemeris_data, line);

            // Filter whitespace from the beginning of the line
            scan = line;
            while ((*scan > '\0') && (*scan <= ' ')) scan++;

            // Ignore blank lines
            if (scan[0] == '\0') continue;

            // Ignore comment lines
            if (scan[0] == '#') continue;

            // Read columns of data output from the ephemeris generator
            double jd = get_float(scan, NULL); // Julian day number
            scan = next_word(scan);
            double ra = get_float(scan, NULL); // radians
            scan = next_word(scan);
            double dec = get_float(scan, NULL); // radians

            // Store this data point into s->ephemeris_data
            s->ephemeris_data[i].data[line_counter].ra = ra;
            s->ephemeris_data[i].data[line_counter].dec = dec;
            s->ephemeris_data[i].data[line_counter].text_label = NULL;
            s->ephemeris_data[i].data[line_counter].day = 0;
            s->ephemeris_data[i].data[line_counter].month = 0;
            s->ephemeris_data[i].data[line_counter].year = 0;
            s->ephemeris_data[i].data[line_counter].sub_month_label = 0;

            // Extract calendar date components for this ephemeris data point
            int year, month, day, hour, minute, status;
            double second;
            inv_julian_day(jd, &year, &month, &day, &hour, &minute, &second, &status, temp_err_string);

            // Create a text label for this point on the ephemeris track
            int sub_month_label = 0;
            if (day > 6) {
                // Within each month, place labels at weekly intervals
                snprintf(label, FNAME_LENGTH, "%.0f", floor(day / 7.) * 7); // Show day of month only
                sub_month_label = 1;
            } else if ((month == 1) || (previous_label[0] == '\0')) {
                // In January, and in the first label on an ephemeris, include the year
                snprintf(label, FNAME_LENGTH, "%.3s %d", get_month_name(month), year); // e.g. Jan 2022
            } else {
                // Omit the year in subsequent new months within the same year
                snprintf(label, FNAME_LENGTH, "%.3s", get_month_name(month)); // e.g. Aug
            }

            // Decide whether to show this label. Do so if we've just entered a new month.
            // If we've not yet put any labels on the ephemeris, wait until the first day of a month to start.
            if ((strncmp(label, previous_label, 3) != 0) && ((previous_label[0] != '\0') || (day == 1))) {
                s->ephemeris_data[i].data[line_counter].text_label = string_make_permanent(label);
                s->ephemeris_data[i].data[line_counter].day = day;
                s->ephemeris_data[i].data[line_counter].month = month;
                s->ephemeris_data[i].data[line_counter].year = year;
                s->ephemeris_data[i].data[line_counter].sub_month_label = sub_month_label;

                // Keep track of the previous label we have shown on this track, which we use to decide when to
                // display a next label
                previous_label = s->ephemeris_data[i].data[line_counter].text_label;
            }

            // Increment data point counter
            line_counter++;
        }

        // Throw an error if we got no data
        if (line_counter == 0) {
            stch_fatal(__FILE__, __LINE__, "ephemeris-compute-de430 returned no data");
            exit(1);
        }

        // Record how many lines of data were returned from ephemeris-compute-de430
        s->ephemeris_data[i].point_count = line_counter;

        // Keep tally of the sum total number of points on all ephemerides
        total_ephemeris_points += s->ephemeris_data[i].point_count;
    }

    // For the purposes of working out minimal sky area encompassing all ephemerides, we concatenate all ephemerides
    // into a single big array
    double *ra_list = (double *) malloc(total_ephemeris_points * sizeof(double));
    double *dec_list = (double *) malloc(total_ephemeris_points * sizeof(double));

    // Loop over all the ephemeris tracks we have computed, and populate the grids <ra_usage> and <dec_usage> with all
    // the cells that any of the moving objects visited
    for (i = j = 0; i < s->ephmeride_count; i++)
        for (k = 0; k < s->ephemeris_data[i].point_count; j++, k++) {
            // Populate the big list of all ephemeris data points
            ra_list[j] = s->ephemeris_data[i].data[k].ra;
            dec_list[j] = s->ephemeris_data[i].data[k].dec;

            // Work out which grid cell this ephemeris point falls within in the grids <ra_usage> and <dec_usage>
            int ra_bin = (int) (ra_list[j] / (2 * M_PI) * ra_bins);
            int dec_bin = (int) ((dec_list[j] + (M_PI / 2)) / M_PI * dec_bins);

            // Mark this point on the map of usage of RA and Dec
            ra_usage[ra_bin] = 1;
            dec_usage[dec_bin] = 1;
            // printf("pt %4d: %.3f %.3f\n", j, ra_list[j] * 12 / M_PI, dec_list[j] * 180 / M_PI);
        }

    // Work out centroid on the sky of all ephemeris data points
    double ra_centroid, dec_centroid;
    find_mean_position(&ra_centroid, &dec_centroid, ra_list, dec_list, total_ephemeris_points);
    // printf("centroid: %.3f %.3f\n", ra_centroid * 12 / M_PI, dec_centroid * 180 / M_PI);

    // Work out which bin the centroid falls within
    int ra_centre_bin = (int) (ra_centroid / (2 * M_PI) * ra_bins);
    // int dec_centre_bin = (int) ((dec_centroid + (M_PI / 2)) / M_PI * dec_bins);

    // Find the RA opposite in the sky to the centroid of all ephemeris
    double ra_anti_centre = ra_centroid + M_PI;
    //double dec_anti_centre = -dec_centroid;

    // Work out which bin of RA/Dec coverage the opposite point falls within
    int ra_anti_centre_bin = (int) (ra_anti_centre / (2 * M_PI) * ra_bins);
    //int dec_anti_centre_bin = (int) ((dec_anti_centre + (M_PI / 2)) / M_PI * dec_bins);

    // Make sure that this falls within the range of the array <ra_usage> (since RA wraps around from 0 to 2pi).
    while (ra_anti_centre_bin < 0) ra_anti_centre_bin += ra_bins;
    while (ra_anti_centre_bin >= ra_bins) ra_anti_centre_bin -= ra_bins;

    // Peel back sky coverage east and west from anti-centre until an ephemeris is reached

    // Find minimum RA used, wrapping around the RA=24h
    int ra_bin_min = ra_anti_centre_bin + 1;
    while (!ra_usage[ra_bin_min]) {
        // If we reach the centroid of the chart, something has gone wrong
        if (ra_bin_min == ra_centre_bin) {
            s->ephemeris_autoscale = 0;
            break;
        }
        ra_bin_min = (ra_bin_min + 1) % ra_bins;
    }

    // Find maximum RA used, wrapping around the RA=24h
    int ra_bin_max = ra_anti_centre_bin;
    while (!ra_usage[ra_bin_max]) {
        // If we reach the centroid of the chart, something has gone wrong
        if (ra_bin_max == ra_centre_bin) {
            s->ephemeris_autoscale = 0;
            break;
        }
        ra_bin_max = (ra_bin_max + ra_bins - 1) % ra_bins;
    }

    // Find southernmost declination used
    int dec_bin_min = 0;
    while (!dec_usage[dec_bin_min]) {
        // If we reach the centroid of the chart, something has gone wrong
        if (dec_bin_min == dec_bins - 1) {
            s->ephemeris_autoscale = 0;
            break;
        }
        dec_bin_min++;
    }

    // Find northernmost declination used
    int dec_bin_max = dec_bins - 1;
    while (!dec_usage[dec_bin_max]) {
        // If we reach the centroid of the chart, something has gone wrong
        if (dec_bin_max == 0) {
            s->ephemeris_autoscale = 0;
            break;
        }
        dec_bin_max--;
    }

    // We have now fully determined the maximum limits of all the ephemerides, north, south, east and west.

    // Convert RA and Dec of the bounding box of the star chart from bin numbers back into angles
    double ra_min = ra_bin_min * 24. / ra_bins;  // hours
    double ra_max = (ra_bin_max + 1) * 24. / ra_bins;  // hours ; ra_bin_max points to the last occupied bin
    double dec_min = dec_bin_min * 180. / dec_bins - 90; // degrees
    double dec_max = (dec_bin_max + 1) * 180. / dec_bins - 90; // degrees

    // Make sure that angles fall within range
    while (ra_max <= ra_min) ra_max += 24;
    while (ra_max > ra_min + 24) ra_max -= 24;
    // printf("RA %.4f %.4f; Dec %.4f %.4f\n", ra_min, ra_max, dec_min, dec_max);

    // Work out maximum angular size of the star chart we need
    double angular_width_base = MAX((ra_max - ra_min) * 180 / 12, dec_max - dec_min) * 1.1;

    // If star chart covers almost the whole sky, it may as well cover the entire sky
    if (angular_width_base > 350) angular_width_base = 360;

    // Report sky coverage
    if (DEBUG) {
        char message[FNAME_LENGTH];
        snprintf(message, FNAME_LENGTH, "  RA  range: %.1fh to %.1fh", ra_min, ra_max);
        stch_log(message);
        snprintf(message, FNAME_LENGTH, "  Dec range: %.1fd to %.1fd", dec_min, dec_max);
        stch_log(message);
        snprintf(message, FNAME_LENGTH, "  Ang width: %.1f deg", angular_width_base);
        stch_log(message);
    }

    // If plot is auto-scaling, set coordinates for the centre and the angular extent
    if (s->ephemeris_autoscale) {
        // The coordinates of the centre of the star chart
        s->ra0 = (ra_min + ra_max) / 2;
        s->dec0 = (dec_min + dec_max) / 2;

        // Make sure that RA is within range
        while (s->ra0 < 0) s->ra0 += 24;
        while (s->ra0 >= 24) s->ra0 -= 24;

        // Don't display the Flamsteed numbers of stars on charts which cover a very large sky area
        if (angular_width_base > 22) s->star_flamsteed_labels = 0;

        // Set an appropriate projection
        if (angular_width_base > 110) {
            // Charts wider than 110 degrees should use a rectangular projection, not a gnomonic projection
            s->projection = SW_PROJECTION_FLAT;
            s->angular_width = angular_width_base;

            // Plots which cover the whole sky need to be really big...
            s->width *= 1.6;
            s->font_size *= 0.95;
            s->mag_min = MIN(s->mag_min, 5);
            s->maximum_star_label_count = 25;
            s->dso_names = 0;

            // Normally use an aspect ratio of 0.5, but if RA span is large and Dec span small, go wide and thin
            s->aspect = MIN(0.5, fabs(dec_max - dec_min) / (fabs(ra_max - ra_min) * 180 / 12) * 1.8);

            // Deal with tall narrow finder charts
            if (fabs(dec_max - dec_min) / (fabs(ra_max - ra_min) * 180 / 12) > 0.5) {
                s->aspect = 1;
                s->width *= 0.7;
            }

            // Make sure that plot does not go outside the declination range -90 to 90
            double ang_height = angular_width_base * s->aspect;
            s->dec0 = MAX(s->dec0, -89 + ang_height / 2);
            s->dec0 = MIN(s->dec0, 89 - ang_height / 2);

        } else {
            // Charts which cover less than 110 degrees should use a gnomonic projection
            s->projection = SW_PROJECTION_GNOM;

            // Pick an attractive aspect ratio for this chart
            s->aspect = ceil(fabs(dec_max - dec_min) / (fabs(ra_max - ra_min) * 180 / 12) * 10.) / 10.;
            if (s->aspect < 0.5) s->aspect = 0.5;
            if (s->aspect > 1.5) s->aspect = 1.5;

            // Fix angular width to take account of the aspect ratio of the plotting area
            double angular_width = MAX((ra_max - ra_min) * 180 / 12, (dec_max - dec_min) / s->aspect) * 1.1;
            if (angular_width > 350) angular_width = 360;
            s->angular_width = angular_width;
        }
    }

    // Free up storage
    free(ra_list);
    free(dec_list);
    free(ra_usage);
    free(dec_usage);
}


//! ephemerides_free - Free memory allocated to store ephemeride data for solar system objects
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.

void ephemerides_free(chart_config *s) {
    int i;
    for (i = 0; i < s->ephmeride_count; i++) {
        free(s->ephemeris_data[i].data);
    }
    free(s->ephemeris_data);
}

//! plot_ephemeris - Plot an ephemeris for a solar system object.
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.
//! \param ld - A <line_drawer> structure used to draw lines on a cairo surface.
//! \param page - A <cairo_page> structure defining the cairo drawing context.
//! \param trace_num - The number of the ephemeris to draw (an index within <s->ephemeris_definitions>).

void plot_ephemeris(chart_config *s, line_drawer *ld, cairo_page *page, int trace_num) {
    int i;
    double last_x = 0, last_y = 0, initial_theta = 0.0;

    // Set line colour
    ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);
    cairo_set_source_rgb(s->cairo_draw, s->ephemeris_col.red, s->ephemeris_col.grn, s->ephemeris_col.blu);
    ld_label(ld, NULL, 1, 1);

    // Loop over the points in the ephemeris, and draw a line across the star chart
    const ephemeris *e = &s->ephemeris_data[trace_num];
    for (i = 0; i < e->point_count; i++) {
        double x, y;

        // Work out the coordinates of each ephemeris data point on the plotting canvas
        plane_project(&x, &y, s, e->data[i].ra, e->data[i].dec, 0);
        if ((x < s->x_min) || (x > s->x_max) || (y < s->y_min) || (y > s->y_max)) continue;

        // Add this point to the line we are tracing
        ld_point(ld, x, y, NULL);

        // Store initial direction of ephemeris track to use later when drawing ticks perpendicular to it
        if (i == 2) initial_theta = atan2(y - last_y, x - last_x);
        last_x = x;
        last_y = y;
    }

    // We have finished tracing ephemeris line, so lift the pen
    ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);

    // Then draw tick marks to indicate notable points along the path of the object
    for (i = 0; i < e->point_count; i++) {
        double x, y, theta;

        // Work out how long this tick mark should be; major time points get longer ticks
        const double physical_tick_len = e->data[i].sub_month_label ? 0.12 : 0.2; // cm
        const double line_width = e->data[i].sub_month_label ? 0.8 : 2;
        const double graph_coords_tick_len = physical_tick_len * s->wlin / s->width;

        cairo_set_line_width(s->cairo_draw, line_width * s->line_width_base);

        // Work out coordinates of this tick mark on the plotting canvas
        plane_project(&x, &y, s, e->data[i].ra, e->data[i].dec, 0);

        // Work out direction of ephemeris track
        if (i < 2) theta = initial_theta;
        else theta = atan2(y - last_y, x - last_x);

        // This happens when ephemeris track is the same twice running; deal gracefully with it
        if (!gsl_finite(theta)) theta = 0.0;

        last_x = x;
        last_y = y;

        // Add point to label exclusion region so that labels don't collide with it
        page->exclusion_regions[page->exclusion_region_counter].x_min = x - graph_coords_tick_len * 0.1;
        page->exclusion_regions[page->exclusion_region_counter].x_max = x + graph_coords_tick_len * 0.1;
        page->exclusion_regions[page->exclusion_region_counter].y_min = y - graph_coords_tick_len * 0.1;
        page->exclusion_regions[page->exclusion_region_counter].y_max = y + graph_coords_tick_len * 0.1;
        page->exclusion_region_counter++;

        // Make tick mark
        if (e->data[i].text_label != NULL) {
            int h_align, v_align;
            const double theta_deg = theta * 180 / M_PI;

            // Reject this tick mark if it's off the side of the star chart
            if ((x < s->x_min) || (x > s->x_max) || (y < s->y_min) || (y > s->y_max)) continue;

            // Add tick mark to label exclusion region so that labels don't collide with it
            page->exclusion_regions[page->exclusion_region_counter].x_min = x - graph_coords_tick_len * 0.4;
            page->exclusion_regions[page->exclusion_region_counter].x_max = x + graph_coords_tick_len * 0.4;
            page->exclusion_regions[page->exclusion_region_counter].y_min = y - graph_coords_tick_len * 0.4;
            page->exclusion_regions[page->exclusion_region_counter].y_max = y + graph_coords_tick_len * 0.4;
            page->exclusion_region_counter++;

            // Draw tick mark
            ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);
            ld_label(ld, NULL, 1, 1);
            ld_point(ld, x + graph_coords_tick_len * sin(theta), y - graph_coords_tick_len * cos(theta), NULL);
            ld_point(ld, x - graph_coords_tick_len * sin(theta), y + graph_coords_tick_len * cos(theta), NULL);
            ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);

            // Work out horizontal and vertical alignment of this label, based on the direction the ephemeris is
            // travelling in. From this, decide how the tick label text should be aligned relative to the end of the
            // tick marker.
            if (theta_deg < -135 - 22.5) {
                h_align = 0; // centre
                v_align = -1; // middle
            } else if (theta_deg < -90 - 22.5) {
                h_align = 1; // right
                v_align = -1;
            } else if (theta_deg < -45 - 22.5) {
                h_align = 1;
                v_align = 0;
            } else if (theta_deg < -22.5) {
                h_align = 1;
                v_align = 1;
            } else if (theta_deg < +22.5) {
                h_align = 0;
                v_align = 1;
            } else if (theta_deg < 45 + 22.5) {
                h_align = -1; // left
                v_align = 1;
            } else if (theta_deg < 90 + 22.5) {
                h_align = -1;
                v_align = 0;
            } else if (theta_deg < 135 + 22.5) {
                h_align = -1;
                v_align = -1;
            } else {
                h_align = 0;
                v_align = -1;
            }

            // Offer the renderer four possible positions where the tick text can be placed

            // Two points, one on either end of the tick marker
            const double label_gap_1 = 1.5;
            const double xp_a = x + label_gap_1 * graph_coords_tick_len * sin(theta);
            const double yp_a = y - label_gap_1 * graph_coords_tick_len * cos(theta);
            const double xp_b = x - label_gap_1 * graph_coords_tick_len * sin(theta);
            const double yp_b = y + label_gap_1 * graph_coords_tick_len * cos(theta);

            // Two further points, also on either end of the tick marker, but this time further out
            const double label_gap_2 = 1.85;
            const double xp_c = x + label_gap_2 * graph_coords_tick_len * sin(theta);
            const double yp_c = y - label_gap_2 * graph_coords_tick_len * cos(theta);
            const double xp_d = x - label_gap_2 * graph_coords_tick_len * sin(theta);
            const double yp_d = y + label_gap_2 * graph_coords_tick_len * cos(theta);

            // Prioritise labels at start of years and quarters
            const double priority = (0.0123 +
                                     1e-12 * i -
                                     4e-6 * (!e->data[i].sub_month_label) -
                                     1e-7 * (e->data[i].day == 14) -
                                     3e-7 * (e->data[i].month == 1) -
                                     2e-7 * (e->data[i].month == 7) -
                                     1e-7 * ((e->data[i].month == 4) || (e->data[i].month == 11))
            );

            // Write text label
            const double font_size = e->data[i].sub_month_label ? 1.3 : 1.7;
            const double extra_margin = e->data[i].sub_month_label ? 2 : 0;
            chart_label_buffer(page, s, s->ephemeris_col, e->data[i].text_label,
                               (label_position[4]) {
                                       {xp_a, yp_a, 0, h_align,  v_align},
                                       {xp_b, yp_b, 0, -h_align, -v_align},
                                       {xp_c, yp_c, 0, h_align,  v_align},
                                       {xp_d, yp_d, 0, -h_align, -v_align}
                               }, 4,
                               0, 1, font_size, 1, 0,
                               extra_margin, priority);
        }
    }
}
