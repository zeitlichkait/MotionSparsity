/**
 * @ Author: Kai Xu
 * @ Create Time: 2020-05-16 11:46:16
 * @ Modified by: Kai Xu
 * @ Modified time: 2020-05-25 22:56:05
 * @ Description: split dense tensor to three sparse tensors with hierarchy of different depths.
 */

#include <pybind11/pybind11.h>
#include <torch/extension.h>
#include "quadtree.hpp"
#include "densetoquad.hpp"

namespace ms
{

    //input Tensor in
    //output Tensor out1 with only first layer.
    //output Tensor out2 with only second layer.
    //output Tensor out3 with only third layer.
    //template <typename Dtype>
    void DenseSplitForwardCPU(at::Tensor &input_r, at::Tensor &out_l1_r,
                              at::Tensor &out_l2_r, at::Tensor &out_l3_r, at::Tensor &out_l4_r, quadtree *structures[])
    {
        //please make sure out_l* are zero tensor.
        auto input = input_r;
        auto out_l1 = out_l1_r;
        auto out_l2 = out_l2_r;
        auto out_l3 = out_l3_r;
        auto out_l4 = out_l4_r;

        auto dim = input.ndimension();

        TORCH_CHECK(dim == 4, "MotionSparsityError: expected 3D tensor, but got tensor with ", dim, " dimensions instead");
        TORCH_CHECK(input.sizes() == out_l1.sizes(), "MotionSparsityError: expected dst and src tensors have the same shape");
        TORCH_CHECK(input.sizes() == out_l2.sizes(), "MotionSparsityError: expected dst and src tensors have the same shape");
        TORCH_CHECK(input.sizes() == out_l3.sizes(), "MotionSparsityError: expected dst and src tensors have the same shape");
        TORCH_CHECK(input.sizes() == out_l4.sizes(), "MotionSparsityError: expected dst and src tensors have the same shape");

        input = input.contiguous();
        out_l1 = out_l1.contiguous();
        out_l2 = out_l2.contiguous();
        out_l3 = out_l3.contiguous();
        out_l4 = out_l4.contiguous();

        auto T = input.size(0);
        auto f = input.size(1);
        auto h = input.size(2);
        auto w = input.size(3);
        at::parallel_for(0, T, 0, [&](int64_t start, int64_t end) {
            for (auto t = start; t < end; t++)
            {
                auto stru = structures[t];
                auto input_t = input[t];
                auto out_l1_t = out_l1[t];
                auto out_l2_t = out_l2[t];
                auto out_l3_t = out_l3[t];
                auto out_l4_t = out_l4[t];

                //create from dense
                quadtree *input_quad;
                input_quad = DenseToQuad(f, h, w, input_t.data_ptr<float>(), stru);

                //split to three tensor with padding
                splitQuadToDense(f, h, w, input_quad, out_l1_t.data_ptr<float>(), out_l2_t.data_ptr<float>(), out_l3_t.data_ptr<float>(), out_l4_t.data_ptr<float>());

                get_padded_tensor(out_l1_t, input_t);
                get_padded_tensor(out_l2_t, input_t);
                get_padded_tensor(out_l3_t, input_t);
                get_padded_tensor(out_l4_t, input_t);
                //dtor 想想怎么写
            }
        });
    }

    void splitQuadToDense(const int &f, const int &tensor_h, const int &tensor_w, quadtree *input_quad, float *out_l1_dst, float *out_l2_dst, float *out_l3_dst, float *out_l4_dst)
    {
        int n_blocks = input_quad->num_blocks();
        int grid_height = input_quad->grid_height;
        int grid_width = input_quad->grid_width;
        int feature_size = input_quad->feature_size;

        assert(f == feature_size && ((float)tensor_h / input_quad->grid_height) == ((float)input_quad->grid_width / tensor_w) &&
               "expect input structure has same size with data tensor.");
        float scale_factor = (float)tensor_h / grid_height;
        //#pragma omp parallel for
        for (int grid_idx = 0; grid_idx < n_blocks; ++grid_idx)
        {
            bitset<21UL> &grid_tree = input_quad->trees[grid_idx];
            qt_data_t *grid_data = input_quad->data + input_quad->feature_size * input_quad->prefix_leafs[grid_idx];

            int grid_h_idx = grid_idx / grid_width;
            int grid_w_idx = grid_idx % grid_width;
            float centre_x = grid_w_idx * 8 + 4;
            float centre_y = grid_h_idx * 8 + 4;

            if (tree_isset_bit(grid_tree, 0))
            {
                for (int hl1 = 0; hl1 < 2; ++hl1)
                {
                    for (int wl1 = 0; wl1 < 2; ++wl1)
                    {
                        int bit_idx_l1 = 1 + hl1 * 2 + wl1;
                        float centre_x_l1 = centre_x + (wl1 * 4) - 2;
                        float centre_y_l1 = centre_y + (hl1 * 4) - 2;
                        if (tree_isset_bit(grid_tree, bit_idx_l1))
                        {
                            for (int hl2 = 0; hl2 < 2; ++hl2)
                            {
                                for (int wl2 = 0; wl2 < 2; ++wl2)
                                {
                                    int bit_idx_l2 = child_idx(bit_idx_l1) + hl2 * 2 + wl2;
                                    float centre_x_l2 = centre_x_l1 + (wl2 * 2) - 1;
                                    float centre_y_l2 = centre_y_l1 + (hl2 * 2) - 1;
                                    if (tree_isset_bit(grid_tree, bit_idx_l2))
                                    {
                                        for (int hl3 = 0; hl3 < 2; ++hl3)
                                        {
                                            for (int wl3 = 0; wl3 < 2; ++wl3)
                                            {
                                                int bit_idx_l3 = child_idx(bit_idx_l2) + hl3 * 2 + wl3;
                                                float centre_x_l3 = centre_x_l2 + (wl3 * 1) - 0.5;
                                                float centre_y_l3 = centre_y_l2 + (hl3 * 1) - 0.5;
                                                int data_idx = tree_data_idx(grid_tree, bit_idx_l3, feature_size);
                                                get_data_from_tensor(grid_data + data_idx, out_l4_dst, scale_factor, tensor_h, tensor_w, feature_size, centre_x_l2 - 0.5, centre_x_l2 + 0.5, centre_y_l2 - 0.5, centre_y_l2 + 0.5);
                                            }
                                        }
                                    }
                                    else
                                    {
                                        int data_idx = tree_data_idx(grid_tree, bit_idx_l2, feature_size);
                                        save_data_to_tensor(grid_data + data_idx, out_l3_dst, scale_factor, tensor_h, tensor_w, feature_size, centre_x_l2 - 1, centre_x_l2 + 1, centre_y_l2 - 1, centre_y_l2 + 1);
                                    }
                                }
                            }
                        }
                        else
                        {
                            int data_idx = tree_data_idx(grid_tree, bit_idx_l1, feature_size);
                            save_data_to_tensor(grid_data + data_idx, out_l2_dst, scale_factor, tensor_h, tensor_w, feature_size, centre_x_l1 - 2, centre_x_l1 + 2, centre_y_l1 - 2, centre_y_l1 + 2);
                        }
                    }
                }
            }
            else
            {
                //ouput whole grid(cx-4,cx+4,cy-4,cy+4) to out_l1_dst tensor
                save_data_to_tensor(grid_data, out_l1_dst, scale_factor, tensor_h, tensor_w, feature_size, centre_x - 4, centre_x + 4, centre_y - 4, centre_y + 4);
            }
        }
    }
    void save_data_to_tensor(qt_data_t *src_data, float *dst_tensor, const float &scale_factor, const int &tensor_h, const int &tensor_w, int &feature_size, const float &h1, const float &h2, const float &w1, const float &w2)
    {
        //data_ptr accessor: f_index*(h*w) + h_index*w + w_index
        //do pooling into one leaf

        int h1_tensor = int(h1 * scale_factor);
        int h2_tensor = int(h2 * scale_factor);
        int w1_tensor = int(w1 * scale_factor);
        int w2_tensor = int(w2 * scale_factor);

        for (int h = h1_tensor; h < h2_tensor; ++h)
        {
            for (int w = w1_tensor; w < w2_tensor; ++w)
            {
                for (int f = 0; f < feature_size; ++f)
                {
                    float val;

                    dst_tensor[(f * tensor_h + h) * tensor_w + w] = src_data[f];
                }
            }
        }
    }

    void get_padded_tensor(at::Tensor &input_tensor, at::Tensor &ref)
    {
        //if the position in the input != 0;
        //then check its neighbor
        //it == 0
        //then pad it (i.e. get the value from the ref tensor)
        auto input_acc = input_tensor.accessor<float, 3>();
        auto ref_acc = ref.accessor<float, 3>();
        int nf = input_tensor.size(0);
        int nx = input_tensor.size(1);
        int ny = input_tensor.size(2);

        for (int f = 0; f < nf; ++f)
        {
            for (int x = 0; x < nx; ++x)
            {
                for (int y = 0; y < ny; ++y)
                {
                    if (input_acc[f][x][y] != 0)
                    {
                        if (x == 0 && y == 0)
                        {
                            if (input_acc[f][x + 1][y] == 0)
                            {
                                input_acc[f][x + 1][y] = ref_acc[f][x + 1][y];
                            }
                            if (input_acc[f][x][y + 1] == 0)
                            {
                                input_acc[f][x][y + 1] = ref_acc[f][x][y + 1];
                            }
                        }
                        else if (x == 0 && y == ny - 1)
                        {
                            if (input_acc[f][x + 1][y] == 0)
                            {
                                input_acc[f][x + 1][y] = ref_acc[f][x + 1][y];
                            }
                            if (input_acc[f][x][y - 1] == 0)
                            {
                                input_acc[f][x][y - 1] = ref_acc[f][x][y - 1];
                            }
                        }
                        else if (x == nx - 1 && y == 0)
                        {
                            if (input_acc[f][x - 1][y] == 0)
                            {
                                input_acc[f][x - 1][y] = ref_acc[f][x - 1][y];
                            }
                            if (input_acc[f][x][y + 1] == 0)
                            {
                                input_acc[f][x][y + 1] = ref_acc[f][x][y + 1];
                            }
                        }
                        else if (x == nx - 1 && y == ny - 1)
                        {
                            if (input_acc[f][x - 1][y] == 0)
                            {
                                input_acc[f][x - 1][y] = ref_acc[f][x - 1][y];
                            }
                            if (input_acc[f][x][y - 1] == 0)
                            {
                                input_acc[f][x][y - 1] = ref_acc[f][x][y - 1];
                            }
                        }
                        else
                        {
                            if (input_acc[f][x - 1][y] == 0)
                            {
                                input_acc[f][x - 1][y] = ref_acc[f][x - 1][y];
                            }
                            if (input_acc[f][x + 1][y] == 0)
                            {
                                input_acc[f][x + 1][y] = ref_acc[f][x + 1][y];
                            }
                            if (input_acc[f][x][y - 1] == 0)
                            {
                                input_acc[f][x][y - 1] = ref_acc[f][x][y - 1];
                            }
                            if (input_acc[f][x][y + 1] == 0)
                            {
                                input_acc[f][x][y + 1] = ref_acc[f][x][y + 1];
                            }
                        }
                    }
                }
            }
        }
    }

} // namespace ms
