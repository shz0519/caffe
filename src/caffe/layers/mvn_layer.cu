#include <algorithm>
#include <vector>

#include "caffe/common_layers.hpp"
#include "caffe/layer.hpp"
#include "caffe/util/math_functions.hpp"

namespace caffe {

template <typename Dtype>
__global__ void FixNegVar(const int n, const Dtype* in,
                                     Dtype* out, Dtype epsilon) {
  CUDA_KERNEL_LOOP(index, n) {
    out[index] = in[index] < epsilon ? epsilon : in[index];
  }
}

template <typename Dtype>
void MVNLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  const Dtype* bottom_data = bottom[0]->gpu_data();
  Blob<Dtype>* top_blob = blob_helper_.DataBlob(top);
  Dtype* top_data = top_blob->mutable_gpu_data();
  int num;
  if (this->layer_param_.mvn_param().across_channels())
    num = bottom[0]->num();
  else
    num = bottom[0]->num() * bottom[0]->channels();

  int dim = bottom[0]->count() / num;

  if (this->layer_param_.mvn_param().normalize_variance()) {
    // put the squares of bottom into temp_
    caffe_gpu_powx(bottom[0]->count(), bottom_data, Dtype(2),
        temp_.mutable_gpu_data());

    // computes variance using var(X) = E(X^2) - (EX)^2
    caffe_gpu_gemv<Dtype>(CblasNoTrans, num, dim, 1. / dim, bottom_data,
        sum_multiplier_.gpu_data(), 0., mean_.mutable_gpu_data());  // EX
    caffe_gpu_gemv<Dtype>(CblasNoTrans, num, dim, 1. / dim, temp_.gpu_data(),
        sum_multiplier_.gpu_data(), 0.,
        variance_.mutable_gpu_data());  // E(X^2)
    caffe_gpu_powx(mean_.count(), mean_.gpu_data(), Dtype(2),
        temp_.mutable_gpu_data());  // (EX)^2
    caffe_gpu_sub(mean_.count(), variance_.gpu_data(), temp_.gpu_data(),
        variance_.mutable_gpu_data());  // variance

    // Check for, and correct, slightly negative variance numbers which can
    // happen when true variance is zero (e.g. solid color image) due to float
    // roundoff error.
    const int count = variance_.count();
    // NOLINT_NEXT_LINE(whitespace/operators)
    FixNegVar<Dtype><<<CAFFE_GET_BLOCKS(count), CAFFE_CUDA_NUM_THREADS >>>(
        variance_.count(), variance_.gpu_data(),
        variance_.mutable_gpu_data(), eps_);

    // do mean and variance normalization
    // subtract mean
    caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, -1.,
            mean_.gpu_data(), sum_multiplier_.gpu_data(), 0.,
            temp_.mutable_gpu_data());

    caffe_gpu_add(temp_.count(), bottom_data, temp_.gpu_data(), top_data);

    // normalize variance
    caffe_gpu_powx(variance_.count(), variance_.gpu_data(), Dtype(0.5),
          variance_.mutable_gpu_data());

    caffe_gpu_add_scalar(variance_.count(), eps_, variance_.mutable_gpu_data());

    caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, 1.,
          variance_.gpu_data(), sum_multiplier_.gpu_data(), 0.,
          temp_.mutable_gpu_data());

    caffe_gpu_div(temp_.count(), top_data, temp_.gpu_data(), top_data);
    if (blob_helper_.HasVarianceTop()) {
      // If the variance is exported as a top blob, it should just mirror the
      // data in the member mean_ blob.
      blob_helper_.VarianceBlob(top)->ShareData(variance_);
    }
  } else {
    caffe_gpu_gemv<Dtype>(CblasNoTrans, num, dim, 1. / dim, bottom_data,
            sum_multiplier_.gpu_data(), 0., mean_.mutable_gpu_data());  // EX

    // subtract mean
    caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, -1.,
            mean_.gpu_data(), sum_multiplier_.gpu_data(), 0.,
            temp_.mutable_gpu_data());

    caffe_gpu_add(temp_.count(), bottom_data, temp_.gpu_data(), top_data);
  }
  if (blob_helper_.HasMeanTop()) {
    // If the mean is exported as a top blob, it should just mirror the
    // data in the member mean_ blob.
    blob_helper_.MeanBlob(top)->ShareData(mean_);
  }
}


template <typename Dtype>
void MVNLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down,
    const vector<Blob<Dtype>*>& bottom) {

  Blob<Dtype>* top_blob = blob_helper_.DataBlob(top);
  const Dtype* top_diff = top_blob->gpu_diff();
  const Dtype* top_data = top_blob->gpu_data();
  const Dtype* bottom_data = bottom[0]->gpu_data();
  Dtype* bottom_diff = bottom[0]->mutable_gpu_diff();

  int num;
  if (this->layer_param_.mvn_param().across_channels())
    num = bottom[0]->num();
  else
    num = bottom[0]->num() * bottom[0]->channels();

  int dim = bottom[0]->count() / num;

  if (this->layer_param_.mvn_param().normalize_variance()) {
    caffe_gpu_mul(temp_.count(), top_data, top_diff, bottom_diff);
    caffe_gpu_gemv<Dtype>(CblasNoTrans, num, dim, 1., bottom_diff,
          sum_multiplier_.gpu_data(), 0., mean_.mutable_gpu_data());
    caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, 1.,
          mean_.gpu_data(), sum_multiplier_.gpu_data(), 0.,
          bottom_diff);
    caffe_gpu_mul(temp_.count(), top_data, bottom_diff, bottom_diff);

    caffe_gpu_gemv<Dtype>(CblasNoTrans, num, dim, 1., top_diff,
            sum_multiplier_.gpu_data(), 0., mean_.mutable_gpu_data());
    caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, 1.,
            mean_.gpu_data(), sum_multiplier_.gpu_data(), 1.,
            bottom_diff);

    caffe_gpu_axpby(temp_.count(), Dtype(1), top_diff, Dtype(-1. / dim),
        bottom_diff);

    // put the squares of bottom into temp_
    caffe_gpu_powx(temp_.count(), bottom_data, Dtype(2),
        temp_.mutable_gpu_data());

    caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, 1.,
        variance_.gpu_data(), sum_multiplier_.gpu_data(), 0.,
        temp_.mutable_gpu_data());

    caffe_gpu_div(temp_.count(), bottom_diff, temp_.gpu_data(), bottom_diff);
  } else {
    caffe_gpu_gemv<Dtype>(CblasNoTrans, num, dim, 1. / dim, top_diff,
            sum_multiplier_.gpu_data(), 0., mean_.mutable_gpu_diff());  // EX

    caffe_gpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, -1.,
            mean_.gpu_diff(), sum_multiplier_.gpu_data(), 0.,
            temp_.mutable_gpu_diff());

    caffe_gpu_add(temp_.count(), top_diff, temp_.gpu_diff(), bottom_diff);
  }
}

INSTANTIATE_LAYER_GPU_FUNCS(MVNLayer);

}  // namespace caffe
