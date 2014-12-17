#include <algorithm>
#include <vector>

#include "caffe/common_layers.hpp"
#include "caffe/layer.hpp"
#include "caffe/util/math_functions.hpp"

namespace caffe {

template <typename Dtype>
void MVNLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype> *> &bottom,
                            const vector<Blob<Dtype> *> &top,
                            const BlobInfo<Dtype> *blob_info) {
  CHECK(layer_param_.has_mvn_param()) << "MVN parameter not specified in "
                                         "layer " << layer_param_.name();
  const MVNParameter& param = layer_param_.mvn_param();
  // If the parameter specifies that the variance blob should be added to the
  // vector of top blobs, then the parameter must also specificy that
  // variance is to be normalized.
  CHECK(param.has_variance() && !param.normalize_variance())
    << "MVNLayer " << layer_param_.name() << " specifies a top blob name for "
       << " the variance blob, but does not normalize for variance.";
  blob_helper_ = MvnBlobOrdering( layer_param_, blob_info, top);
}

// This function causes the member mean_ or variance_ blob to share data
// with the top or bottom blob containing the mean or variance.
// internal blob is the member variable blob mean_. External blob is the blob
// in the top or bottom vector of blobs (now owned by the layer).
void UseExternal(Blob<Dtype>* internal_blob,
                 Blob<Dtype>* external_blob,
                 int num,
                 int channels)
{
  // Before ShareData, the two blobs have to have the same shape.
  external_blob->Reshape(num,channels,1,1);
  blob->ReshapeLike(*external_blob);
  blob->ShareData(*external_blob);
  blob->ShareDiff(*external_blob);
}


template <typename Dtype>
void MVNLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {

  Blob<Dtype>* input_blob = bottom[0];

  // Reshape the data output blob, which is always in the top vector.
  blob_helper_.DataBlob(top)->Reshape(input_blob->num(), input_blob->channels(),
      input_blob->height(), input_blob->width());


  if (blob_helper_.HasMean())
  {
    // If the mean_ is exported as a top blob, have the mean_ member share the
    // data of the mean blob in the top vector.
    Blob<Dtype>* mean_blob = blob_helper_.MeanBlob(top);
    UseExternal( &mean_, blob_helper_.MeanBlob(top), input_blob, mean_blob );
  }
  else
  {
    mean_.Reshape(input_blob->num(), input_blob->channels(), 1, 1);
  }

  if (blob_helper_.HasVariance())
  {
    // If variance_ is exported as a top blob, have it share the
    // data of the variance blob in the top vector.
    Blob<Dtype>* mean_blob = blob_helper_.VarianceBlob(top);
    UseExternal( &variance_, blob_helper_.VarianceBlob(top),
                 input_blob, mean_blob );
  } else {
    variance_.Reshape(input_blob->num(), input_blob->channels(), 1, 1);
  }

  temp_.Reshape(input_blob->num(), input_blob->channels(),
      input_blob->height(), input_blob->width());
  sum_multiplier_.Reshape(1, 1, input_blob->height(), input_blob->width());
  Dtype* multiplier_data = sum_multiplier_.mutable_cpu_data();
  caffe_set(sum_multiplier_.count(), Dtype(1), multiplier_data);
}

template <typename Dtype>
void MVNLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  const Dtype* bottom_data = bottom[0]->cpu_data();
  Dtype* top_data = top[0]->mutable_cpu_data();
  int num;
  if (this->layer_param_.mvn_param().across_channels())
    num = bottom[0]->num();
  else
    num = bottom[0]->num() * bottom[0]->channels();

  int dim = bottom[0]->count() / num;
  Dtype eps = 1e-10;

  if (this->layer_param_.mvn_param().normalize_variance()) {
    // put the squares of bottom into temp_
    caffe_powx(bottom[0]->count(), bottom_data, Dtype(2),
        temp_.mutable_cpu_data());

    // computes variance using var(X) = E(X^2) - (EX)^2
    caffe_cpu_gemv<Dtype>(CblasNoTrans, num, dim, 1. / dim, bottom_data,
        sum_multiplier_.cpu_data(), 0., mean_.mutable_cpu_data());  // EX
    caffe_cpu_gemv<Dtype>(CblasNoTrans, num, dim, 1. / dim, temp_.cpu_data(),
        sum_multiplier_.cpu_data(), 0.,
        variance_.mutable_cpu_data());  // E(X^2)
    caffe_powx(mean_.count(), mean_.cpu_data(), Dtype(2),
        temp_.mutable_cpu_data());  // (EX)^2
    caffe_sub(mean_.count(), variance_.cpu_data(), temp_.cpu_data(),
        variance_.mutable_cpu_data());  // variance

    // do mean and variance normalization
    // subtract mean
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, -1.,
            mean_.cpu_data(), sum_multiplier_.cpu_data(), 0.,
            temp_.mutable_cpu_data());

    caffe_add(temp_.count(), bottom_data, temp_.cpu_data(), top_data);

    // normalize variance
    caffe_powx(variance_.count(), variance_.cpu_data(), Dtype(0.5),
          variance_.mutable_cpu_data());

    caffe_add_scalar(variance_.count(), eps, variance_.mutable_cpu_data());

    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, 1.,
          variance_.cpu_data(), sum_multiplier_.cpu_data(), 0.,
          temp_.mutable_cpu_data());

    caffe_div(temp_.count(), top_data, temp_.cpu_data(), top_data);
  } else {
    caffe_cpu_gemv<Dtype>(CblasNoTrans, num, dim, 1. / dim, bottom_data,
            sum_multiplier_.cpu_data(), 0., mean_.mutable_cpu_data());  // EX

    // subtract mean
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, -1.,
            mean_.cpu_data(), sum_multiplier_.cpu_data(), 0.,
            temp_.mutable_cpu_data());

    caffe_add(temp_.count(), bottom_data, temp_.cpu_data(), top_data);
  }
}

template <typename Dtype>
void MVNLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down,
    const vector<Blob<Dtype>*>& bottom) {
  const Dtype* top_diff = top[0]->cpu_diff();
  const Dtype* top_data = top[0]->cpu_data();
  const Dtype* bottom_data = bottom[0]->cpu_data();
  Dtype* bottom_diff = bottom[0]->mutable_cpu_diff();

  int num;
  if (this->layer_param_.mvn_param().across_channels())
    num = bottom[0]->num();
  else
    num = bottom[0]->num() * bottom[0]->channels();

  int dim = bottom[0]->count() / num;
  Dtype eps = 1e-10;

  if (this->layer_param_.mvn_param().normalize_variance()) {
    caffe_mul(temp_.count(), top_data, top_diff, bottom_diff);
    caffe_cpu_gemv<Dtype>(CblasNoTrans, num, dim, 1., bottom_diff,
          sum_multiplier_.cpu_data(), 0., mean_.mutable_cpu_data());
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, 1.,
          mean_.cpu_data(), sum_multiplier_.cpu_data(), 0.,
          bottom_diff);
    caffe_mul(temp_.count(), top_data, bottom_diff, bottom_diff);

    caffe_cpu_gemv<Dtype>(CblasNoTrans, num, dim, 1., top_diff,
            sum_multiplier_.cpu_data(), 0., mean_.mutable_cpu_data());
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, 1.,
            mean_.cpu_data(), sum_multiplier_.cpu_data(), 1.,
            bottom_diff);

    caffe_cpu_axpby(temp_.count(), Dtype(1), top_diff, Dtype(-1. / dim),
        bottom_diff);

    // put the squares of bottom into temp_
    caffe_powx(temp_.count(), bottom_data, Dtype(2),
        temp_.mutable_cpu_data());

    // computes variance using var(X) = E(X^2) - (EX)^2
    caffe_cpu_gemv<Dtype>(CblasNoTrans, num, dim, 1. / dim, bottom_data,
        sum_multiplier_.cpu_data(), 0., mean_.mutable_cpu_data());  // EX
    caffe_cpu_gemv<Dtype>(CblasNoTrans, num, dim, 1. / dim, temp_.cpu_data(),
        sum_multiplier_.cpu_data(), 0.,
        variance_.mutable_cpu_data());  // E(X^2)
    caffe_powx(mean_.count(), mean_.cpu_data(), Dtype(2),
        temp_.mutable_cpu_data());  // (EX)^2
    caffe_sub(mean_.count(), variance_.cpu_data(), temp_.cpu_data(),
        variance_.mutable_cpu_data());  // variance

    // normalize variance
    caffe_powx(variance_.count(), variance_.cpu_data(), Dtype(0.5),
          variance_.mutable_cpu_data());

    caffe_add_scalar(variance_.count(), eps, variance_.mutable_cpu_data());

    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, num, dim, 1, 1.,
        variance_.cpu_data(), sum_multiplier_.cpu_data(), 0.,
        temp_.mutable_cpu_data());

    caffe_div(temp_.count(), bottom_diff, temp_.cpu_data(), bottom_diff);
  } else {
    caffe_copy(temp_.count(), top_diff, bottom_diff);
  }
}


#ifdef CPU_ONLY
STUB_GPU(MVNLayer);
#endif

INSTANTIATE_CLASS(MVNLayer);
REGISTER_LAYER_CLASS(MVN);

}  // namespace caffe
