#include <algorithm>
#include <cfloat>
#include <vector>

#include "thrust/device_vector.h"

#include "caffe/layer.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/vision_layers.hpp"

namespace caffe {

template <typename Dtype>
__global__ void kernel_channel_max_sma(const int num,
                                   const int channels,
                                   const int spatial_dim,
                                   const int softmax_size,
                                   const int num_softmaxes,
                                   const Dtype* data,
                                   Dtype* out) {
  CUDA_KERNEL_LOOP(index, index < num * spatial_dim) {
    int n = index / spatial_dim;
    int s = index % spatial_dim;
    // For each softmax along the canonical axis.
    for( int smi = 0; smi < num_softmaxes; ++smi) {
      Dtype maxval = -FLT_MAX;
      // For each channel within this softmax.
      for (int c_off = 0; c_off < softmax_size; ++c_off) {
        int c = smi * softmax_size + c_off;
        int data_index = (n * channels + c) * spatial_dim + s;
        maxval = max(data[data_index], maxval);
      }
      //int out_index = index*num_softmaxes + smi;
      int out_index = s + (n * num_softmaxes + smi) * spatial_dim ; //index*num_softmaxes + smi;
      out[out_index] = maxval;
    }
  }

}

template <typename Dtype>
__global__ void kernel_channel_subtract_sma(const int count,
                                        const int softmax_size,
                                        const int spatial_dim,
                                        const Dtype* channel_max,
                                        Dtype* data) {
  CUDA_KERNEL_LOOP(index, count) {
    int n = index / softmax_size / spatial_dim;
    int s = index % spatial_dim;

    int softmax_max_index = n * spatial_dim + s;
    //int softmax_index = chanset*num_softmaxes
    data[index] -= channel_max[softmax_max_index];
  }
}

template <typename Dtype>
__global__ void kernel_exp_sma(const int count, const Dtype* data, Dtype* out) {
  CUDA_KERNEL_LOOP(index, count) {
    out[index] = exp(data[index]);
  }
}

template <typename Dtype>
__global__ void kernel_channel_sum_sma(const int num,
                                       const int channels,
                                       const int spatial_dim,
                                       const int softmax_size,
                                       const int num_softmaxes,
                                       const Dtype* data,
                                       Dtype* channel_sum) {
  CUDA_KERNEL_LOOP(index, num * spatial_dim) {
    int n = index / spatial_dim;
    int s = index % spatial_dim;
    // For each softmax along the canonical axis.
    for( int smi = 0; smi < num_softmaxes; ++smi) {
      Dtype sum = 0;
      // For each channel within this softmax.
      for (int c_off = 0; c_off < softmax_size; ++c_off) {
        int c = smi * softmax_size + c_off;
        int data_index = (n * channels + c) * spatial_dim + s;
        sum += data[data_index];
      }
      //int out_index = index*num_softmaxes + smi;
      int out_index = s + (n * num_softmaxes + smi) * spatial_dim ; //index*num_softmaxes + smi;
      channel_sum[out_index] = sum;
    }
  }
}

template <typename Dtype>
__global__ void kernel_channel_div_sma( const int num,
                                    const int channels,
                                    const int spatial_dim,
                                    const int softmax_size,
                                    const int num_softmaxes,
                                    const Dtype* sums,
                                    Dtype* out) {
//  CUDA_KERNEL_LOOP(index, count) {
//    int n = index / channels / spatial_dim;
//    int s = index % spatial_dim;
//    data[index] /= channel_sum[n * spatial_dim + s];
//  }
  //  for( int index = 0; index < num * spatial_dim; ++index)  {
  CUDA_KERNEL_LOOP(index, num*spatial_dim) {
    int n = index / spatial_dim;
    int s = index % spatial_dim;
    // For each softmax along the canonical axis.
    for( int smi = 0; smi < num_softmaxes; ++smi) {
      int sum_index = s + (n * num_softmaxes + smi) * spatial_dim ; //index*num_softmaxes + smi;
      Dtype sum = sums[sum_index];
      // For each channel within this softmax.
      for (int c_off = 0; c_off < softmax_size; ++c_off) {
        int c = smi * softmax_size + c_off;
        int data_index = (n * channels + c) * spatial_dim + s;
        out[data_index] = out[data_index] / sum;
      }
      //int out_index = index*num_softmaxes + smi;
    }
  }
}

template <typename Dtype>
__global__ void kernel_channel_dot(const int num, const int channels,
    const int spatial_dim, const Dtype* data_1, const Dtype* data_2,
    Dtype* channel_dot) {
  CUDA_KERNEL_LOOP(index, num * spatial_dim) {
    int n = index / spatial_dim;
    int s = index % spatial_dim;
    Dtype dot = 0;
    for (int c = 0; c < channels; ++c) {
      dot += (data_1[(n * channels + c) * spatial_dim + s]
          * data_2[(n * channels + c) * spatial_dim + s]);
    }
    channel_dot[index] = dot;
  }
}

template <typename Dtype>
void SoftmaximaLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  int input_count = bottom[0]->count();
  const Dtype* bottom_data = bottom[0]->gpu_data();
  Dtype* top_data = top[0]->mutable_gpu_data();
  Dtype* scale_data = scale_.mutable_gpu_data();
  int count = bottom[0]->count();
  int channels = top[0]->shape(softmax_axis_);
  caffe_copy(count, bottom_data, top_data);
  // We need to subtract the max to avoid numerical issues, compute the exp,
  // and then normalize.
  // compute max
  // NOLINT_NEXT_LINE(whitespace/operators)
  kernel_channel_max_sma<Dtype><<<CAFFE_GET_BLOCKS(outer_num_ * inner_num_),
      CAFFE_CUDA_NUM_THREADS>>>(  outer_num_,
                                  channels,
                                  inner_num_,
                                  softmax_size_,
                                  num_softmaxes_,
                                  top_data,
                                  scale_data);
  // subtract
  // NOLINT_NEXT_LINE(whitespace/operators)
  kernel_channel_subtract_sma<Dtype><<<CAFFE_GET_BLOCKS(input_count),
      CAFFE_CUDA_NUM_THREADS>>>(input_count, softmax_size_, inner_num_,
      scale_data, top_data);
  // exponentiate
  // NOLINT_NEXT_LINE(whitespace/operators)
  kernel_exp_sma<Dtype><<<CAFFE_GET_BLOCKS(input_count), CAFFE_CUDA_NUM_THREADS>>>(
      input_count, top_data, top_data);
  // sum after exp
  // NOLINT_NEXT_LINE(whitespace/operators)
  kernel_channel_sum_sma<Dtype><<<CAFFE_GET_BLOCKS(outer_num_ * inner_num_),
      CAFFE_CUDA_NUM_THREADS>>>(outer_num_,
                                channels,
                                inner_num_,
                                softmax_size_,
                                num_softmaxes_,
                                top_data,
                                scale_data);
  // divide
  // NOLINT_NEXT_LINE(whitespace/operators)
  kernel_channel_div_sma<Dtype><<<CAFFE_GET_BLOCKS(outer_num_*inner_num_),
      CAFFE_CUDA_NUM_THREADS>>>(outer_num_,
                                channels,
                                inner_num_,
                                softmax_size_,
                                num_softmaxes_,
                                scale_data,
                                top_data);
}

template <typename Dtype>
void SoftmaximaLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
//  const Dtype* top_diff = top[0]->gpu_diff();
//  const Dtype* top_data = top[0]->gpu_data();
//  Dtype* bottom_diff = bottom[0]->mutable_gpu_diff();
//  Dtype* scale_data = scale_.mutable_gpu_data();
//  int count = top[0]->count();
//  int channels = top[0]->shape(softmax_axis_);
//  caffe_copy(count, top_diff, bottom_diff);
//  // Compute inner1d(top_diff, top_data) and subtract them from the bottom diff.
//  // NOLINT_NEXT_LINE(whitespace/operators)
//  kernel_channel_dot<Dtype><<<CAFFE_GET_BLOCKS(outer_num_ * inner_num_),
//      CAFFE_CUDA_NUM_THREADS>>>(outer_num_, channels, inner_num_,
//      top_diff, top_data, scale_data);
//  // NOLINT_NEXT_LINE(whitespace/operators)
//  kernel_channel_subtract<Dtype><<<CAFFE_GET_BLOCKS(count),
//      CAFFE_CUDA_NUM_THREADS>>>(count, outer_num_, channels, inner_num_,
//      scale_data, bottom_diff);
//  // elementwise multiplication
//  caffe_gpu_mul<Dtype>(top[0]->count(), bottom_diff, top_data, bottom_diff);
}

INSTANTIATE_LAYER_GPU_FUNCS(SoftmaximaLayer);


}  // namespace caffe
