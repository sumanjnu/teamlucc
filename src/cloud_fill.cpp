#include <RcppArmadillo.h>
#include <Rcpp.h>

using namespace arma;

//' Cloud fill using the algorithm developed by Xiaolin Zhu
//'
//' This function is called by the \code{\link{fill_clouds}} function. It is 
//' not intended to be used directly.
//'
//' @param cloudy the cloudy image, with pixels in columns (in column-major 
//' order) and with number of columns equal to number of bands
//' @param clear the clear image, with pixels in columns (in column-major 
//' order) and with number of columns equal to number of bands
//' @param cloud_mask the cloud mask image as a vector (in column-major order), 
//' with clouds coded with unique integer codes starting at 1, and with areas 
//' that are clear in both images  coded as 0. Areas that are missing in the 
//' clear image, should be coded as -1.
//' @param dims the dimensions of the cloudy image as a length 3 vector: (rows, 
//' columns, bands)
//' @param num_class set the estimated number of classes in image
//' @param min_pixel the sample size of similar pixels
//' @param cloud_nbh the range of cloud neighborhood (in pixels)
//' @param DN_min the minimum valid DN value
//' @param DN_max the maximum valid DN value
//' @return array with cloud filled image with dims: cols, rows, bands
//' parameter, containing the selected textures measures
//' @references Zhu, X., Gao, F., Liu, D., Chen, J., 2012. A modified
//' neighborhood similar pixel interpolator approach for removing thick clouds 
//' in Landsat images. Geoscience and Remote Sensing Letters, IEEE 9, 521--525.
// [[Rcpp::export]]
arma::mat cloud_fill(arma::mat cloudy, arma::mat& clear,
        arma::vec& cloud_mask, arma::vec dims, int num_class,
        int min_pixel, int cloud_nbh, int DN_min, int DN_max) {

    // Allow also treating the multiband images as cubes
    cube cloudy_cube(cloudy.begin(), dims(0), dims(1), dims(2), false);
    cube clear_cube(clear.begin(), dims(0), dims(1), dims(2), false);
    // Allow also treating cloud_mask as 2d matrix (row, cols)
    mat cloud_mask_mat(cloud_mask.begin(), dims(0), dims(1), false);

    vec cloud_codes = unique(cloud_mask);
    // Anything less than 1 is not a cloud code
    cloud_codes = cloud_codes(find(cloud_codes >= 1));

    for(unsigned n=0; n < cloud_codes.n_elem; n++) {
        int cloud_code = cloud_codes(n);

        // These indices refer to the position of cloud pixels within the 
        // overall cloud_mask block
        uvec cloud_vec_i = find(cloud_mask == cloud_code);
        uvec cloud_col_i = floor(cloud_vec_i / dims(0));
        uvec cloud_row_i = cloud_vec_i - cloud_col_i * dims(0);

        // Now add in the cloud neighborhood
        int left_col = min(cloud_col_i) - cloud_nbh;
        if (left_col < 0) left_col = 0;

        int right_col = max(cloud_col_i) + cloud_nbh;
        if (right_col > (dims(1) - 1)) right_col = dims(1) - 1;

        int up_row = min(cloud_row_i) - cloud_nbh;
        if (up_row < 0) up_row = 0;

        int down_row = max(cloud_row_i) + cloud_nbh;
        if (down_row > (dims(0) - 1)) down_row = dims(0) - 1;

        int num_sub_cols = (right_col - left_col) + 1;
        int num_sub_rows = (down_row - up_row) + 1;
        double x_center = num_sub_cols / 2.0;
        double y_center = num_sub_rows / 2.0;

        // Extract the cloud neighborhood from the cubes, and setup 
        // column-major matrices that will be used for the remaining 
        // calculations.
        cube sub_cloudy_cube = cloudy_cube.tube(up_row, left_col, down_row, right_col);
        cube sub_clear_cube = clear_cube.tube(up_row, left_col, down_row, right_col);
        mat sub_cloudy(num_sub_rows*num_sub_cols, dims(2));
        mat sub_clear(num_sub_rows*num_sub_cols, dims(2));
        for (unsigned elnum=0; elnum < sub_clear_cube.n_elem; elnum++) {
            sub_cloudy(elnum) = sub_cloudy_cube(elnum);
            sub_clear(elnum) = sub_clear_cube(elnum);
        }

        mat sub_cloud_mask = cloud_mask_mat.submat(up_row, left_col, down_row, right_col);

        // Compute the threshold for what is a "similar" pixel
        rowvec similar_th_band = stddev(sub_clear, 0, 0) * 2 / num_class;

        // These indices refer to the position of clear pixels within the cloud 
        // neighborhood of this cloud (a subset of clear block)
        uvec sub_clear_vec_i = find(sub_cloud_mask == 0);
        uvec sub_clear_col_i = floor(sub_clear_vec_i / sub_cloud_mask.n_rows);
        uvec sub_clear_row_i = sub_clear_vec_i - sub_clear_col_i * sub_cloud_mask.n_rows;

        mat sub_clear_clear = sub_clear.rows(sub_clear_vec_i);
        mat sub_cloudy_clear = sub_cloudy.rows(sub_clear_vec_i);

        // Below is used when there are NO similar pixels
        rowvec mean_diff = mean(sub_cloudy_clear - sub_clear_clear, 0);

        // These indices refer to the position of pixels of this cloud
        // within the cloud neighborhood of this cloud (a subset of cloud_mask 
        // block)
        uvec sub_cloud_vec_i = find(sub_cloud_mask == cloud_code);
        uvec sub_cloud_col_i = floor(sub_cloud_vec_i / sub_cloud_mask.n_rows);
        uvec sub_cloud_row_i = sub_cloud_vec_i - sub_cloud_col_i * sub_cloud_mask.n_rows;

        for(unsigned ic=0; ic < sub_cloud_vec_i.n_elem; ic++) {
            // Calculate row and column location of target pixel
            int ri = sub_cloud_row_i(ic);
            int ci = sub_cloud_col_i(ic);

            // Calculate distance between target pixel and center of cloud
            double r2 = sqrt(pow(x_center - ri, 2) + pow(y_center - ci, 2));
            // Note need to convert sub_cloud_row_i and sub_cloud_col_i from 
            // type uvec to vec for the below calculations.
            vec clear_dists = sqrt(pow(conv_to<vec>::from(sub_clear_row_i) - ri, 2) +
                                   pow(conv_to<vec>::from(sub_clear_col_i) - ci, 2));

            uvec order_clear = sort_index(clear_dists);
            // Avoids comparing a pixel with itself
            order_clear = order_clear(span(1, order_clear.n_elem - 1));

            // Find similar pixels
            int iclear = 1;
            int num_similar = 0;
            mat cloudy_similar(min_pixel, dims(2));
            mat clear_similar(min_pixel, dims(2));
            vec rmse_similar(min_pixel); // Based on spectral distance
            vec dis_similar(min_pixel); // Based on spatial distance
            while ((num_similar <= (min_pixel-1)) && (iclear <= (order_clear.n_elem - 1))) {
                int indicate_similar = sum((sub_clear_clear.row(order_clear(iclear)) - sub_clear.row(ic)) <= similar_th_band);
                // Below only runs if there are similar pixels in all bands
                if (indicate_similar == dims(2)) {
                    cloudy_similar.row(num_similar) = sub_cloudy_clear.row(order_clear(iclear));
                    clear_similar.row(num_similar) = sub_clear_clear.row(order_clear(iclear));
                    rmse_similar(num_similar) = sqrt(sum(pow(sub_clear_clear.row(order_clear(iclear)) - sub_clear.row(ic), 2))
                            / dims(2));
                    dis_similar(num_similar) = clear_dists(order_clear(iclear));
                    num_similar++;
                }
                iclear++;
            }

            // Perform cloud fill
            if (num_similar > 1) {
                // Need to filter out blank rows if less than min_pixel similar 
                // pixels were found
                if (num_similar < min_pixel) {
                    cloudy_similar = cloudy_similar.rows(span(0, num_similar - 1));
                    clear_similar = clear_similar.rows(span(0, num_similar - 1));
                    rmse_similar = rmse_similar(span(0, num_similar - 1));
                    dis_similar = dis_similar(span(0, num_similar - 1));
                }
                vec rmse_similar_norm = (rmse_similar - min(rmse_similar)) / (max(rmse_similar) - min(rmse_similar) + 0.000001) + 1.0;
                vec dis_similar_norm = (dis_similar - min(dis_similar)) / (max(dis_similar) - min(dis_similar) + 0.000001) + 1.0;
                vec C_D = rmse_similar_norm % dis_similar_norm + 0.0000001;
                vec weight = (1.0 / C_D) / sum(1.0 / C_D);

                // Compute the time weight
                double W_T1 = r2 / (r2 + mean(dis_similar));
                double W_T2 = mean(dis_similar) / (r2 + mean(dis_similar));

                // Make predictions
                mat predict_1 = cloudy_similar;
                predict_1.each_col() %= weight;
                predict_1 = sum(predict_1, 0);

                mat predict_2 = cloudy_similar - clear_similar;
                predict_2.each_col() %= weight;
                predict_2 = sub_clear.row(ic) + sum(predict_2, 0);
                for(unsigned iband=0; iband < dims(2); iband++) {
                    if (predict_2(iband) > DN_min && predict_2(iband) < DN_max) {
                        sub_cloudy(ic, iband) = W_T1 * predict_1(iband) + W_T2 * predict_2(iband);
                    } else {
                        sub_cloudy(ic, iband) = predict_1(iband);
                    }
                }

            } else {
                // If no similar pixel, use mean of all pixels in cloud 
                // neighborhood for a simple linear adjustment
                sub_cloudy.row(ic) = sub_clear.row(ic) + mean_diff;
            }
        }
        // Below is necessary because sub_cloudy is a matrix
        for (unsigned iband=0; iband < dims(2); iband++) {
            mat this_band = sub_cloudy.col(iband);
            this_band.set_size(num_sub_rows, num_sub_cols);
            cloudy_cube(span(up_row, down_row),
                        span(left_col, right_col),
                        span(iband)) = this_band;
        }
    }
    return(cloudy);
}