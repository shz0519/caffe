#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "caffe/blob.hpp"
#include "caffe/blob_finder.hpp"
#include "caffe/common.hpp"
#include "caffe/common_layers.hpp"
#include "caffe/filler.hpp"
#include "caffe/util/io.hpp"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"

#include "caffe/test/test_caffe_main.hpp"
#include "caffe/test/test_gradient_check_util.hpp"

namespace caffe {

template <typename TypeParam>
class MVNLayerTest : public MultiDeviceTest<TypeParam> {
  typedef typename TypeParam::Dtype Dtype;

 protected:
  void AddTopBlob(Blob<Dtype>* blob, const std::string& name) {
    blob_top_vec_.push_back(blob);
    blob_finder_.AddBlob(name, blob);
  }
  MVNLayerTest()
      : blob_bottom_(new Blob<Dtype>(2, 3, 4, 5)),
        blob_top_(new Blob<Dtype>()) {
    // fill the values
    FillerParameter filler_param;
    GaussianFiller<Dtype> filler(filler_param);
    filler.Fill(this->blob_bottom_);
    blob_bottom_vec_.push_back(blob_bottom_);
    AddTopBlob(blob_top_, "top0");
  }
  virtual ~MVNLayerTest() {
    delete blob_bottom_;
    delete blob_top_;
  }

  Blob<Dtype>* const blob_bottom_;
  Blob<Dtype>* const blob_top_;
  vector<Blob<Dtype>*> blob_bottom_vec_;
  vector<Blob<Dtype>*> blob_top_vec_;
  BlobFinder<Dtype> blob_finder_;
};

TYPED_TEST_CASE(MVNLayerTest, TestDtypesAndDevices);

TYPED_TEST(MVNLayerTest, TestForward) {
  typedef typename TypeParam::Dtype Dtype;
  LayerParameter layer_param;
  MVNLayer<Dtype> layer(layer_param);
  layer.SetUp(this->blob_bottom_vec_, this->blob_top_vec_, this->blob_finder_);
  layer.Forward(this->blob_bottom_vec_, this->blob_top_vec_);
  // Test mean
  int num = this->blob_bottom_->num();
  int channels = this->blob_bottom_->channels();
  int height = this->blob_bottom_->height();
  int width = this->blob_bottom_->width();

  for (int i = 0; i < num; ++i) {
    for (int j = 0; j < channels; ++j) {
      Dtype sum = 0, var = 0;
      for (int k = 0; k < height; ++k) {
        for (int l = 0; l < width; ++l) {
          Dtype data = this->blob_top_->data_at(i, j, k, l);
          sum += data;
          var += data * data;
        }
      }
      sum /= height * width;
      var /= height * width;

      const Dtype kErrorBound = 0.001;
      // expect zero mean
      EXPECT_NEAR(0, sum, kErrorBound);
      // expect unit variance
      EXPECT_NEAR(1, var, kErrorBound);
    }
  }

  EXPECT_EQ(this->blob_top_, this->blob_finder_.PointerFromName("top0"));
}

// Test the case where the MVNParameter specifies that the mean and variance
// blobs are to appear in the layer's top blobs.
TYPED_TEST(MVNLayerTest, TestForward_MeanAndVarianceInTopBlobs) {
  typedef typename TypeParam::Dtype Dtype;

  this->AddTopBlob(new Blob<Dtype>(), "mean");
  this->AddTopBlob(new Blob<Dtype>(), "variance");

  LayerParameter layer_param;
  CHECK(google::protobuf::TextFormat::ParseFromString(
      "mvn_param { mean_blob: \"mean\" variance_blob: \"variance\""
      " normalize_variance: true   } "
      " top: \"normalized\" top: \"variance\" top: \"mean\" ", &layer_param));
  MVNLayer<Dtype> layer(layer_param);
  layer.SetUp(this->blob_bottom_vec_, this->blob_top_vec_, this->blob_finder_);
  layer.Forward(this->blob_bottom_vec_, this->blob_top_vec_);
  // Test mean
  int num = this->blob_bottom_->num();
  int channels = this->blob_bottom_->channels();
  int height = this->blob_bottom_->height();
  int width = this->blob_bottom_->width();

  Blob<Dtype> expected_input_means(num, channels, 1, 1);
  Blob<Dtype> expected_input_variances(num, channels, 1, 1);
  for (int i = 0; i < num; ++i) {
    for (int j = 0; j < channels; ++j) {
      Dtype input_mean = 0.0;
      Dtype input_variance = 0.0;
      Dtype sum = 0, var = 0;
      for (int k = 0; k < height; ++k) {
        for (int l = 0; l < width; ++l) {
          Dtype data = this->blob_top_->data_at(i, j, k, l);
          sum += data;
          var += data * data;

          Dtype input_data = this->blob_bottom_->data_at(i, j, k, l);
          input_mean += input_data;
          input_variance += input_data*input_data;
        }
      }
      sum /= height * width;
      var /= height * width;

      Dtype n = height*width;
      input_mean /= n;
      input_variance /= n;
      input_variance -= input_mean*input_mean;
      input_variance = sqrt(input_variance);


      const Dtype kErrorBound = 0.001;
      // expect zero mean
      EXPECT_NEAR(0, sum, kErrorBound);
      // expect unit variance
      EXPECT_NEAR(1, var, kErrorBound);
      *(expected_input_means.mutable_cpu_data() +
          expected_input_means.offset(i, j, 0, 0)) = input_mean;
      *(expected_input_variances.mutable_cpu_data() +
          expected_input_variances.offset(i, j, 0, 0)) = input_variance;
    }
  }
}

// Test the case where the MVNParameter specifies that the mean
// blob is to appear in the layer's top blobs.
TYPED_TEST(MVNLayerTest, TestForward_MeanInTopBlobs) {
  typedef typename TypeParam::Dtype Dtype;

  this->AddTopBlob(new Blob<Dtype>(), "mean");

  LayerParameter layer_param;
  CHECK(google::protobuf::TextFormat::ParseFromString(
      "mvn_param { mean_blob: \"mean\"  }"
      " top: \"normalized\" top: \"mean\" ", &layer_param));
  MVNLayer<Dtype> layer(layer_param);
  layer.SetUp(this->blob_bottom_vec_, this->blob_top_vec_, this->blob_finder_);
  layer.Forward(this->blob_bottom_vec_, this->blob_top_vec_);
  // Test mean
  int num = this->blob_bottom_->num();
  int channels = this->blob_bottom_->channels();
  int height = this->blob_bottom_->height();
  int width = this->blob_bottom_->width();

  Blob<Dtype> expected_input_means(num, channels, 1, 1);
  for (int i = 0; i < num; ++i) {
    for (int j = 0; j < channels; ++j) {
      Dtype input_mean = 0.0;
      Dtype sum = 0, var = 0;
      for (int k = 0; k < height; ++k) {
        for (int l = 0; l < width; ++l) {
          Dtype data = this->blob_top_->data_at(i, j, k, l);
          sum += data;
          var += data * data;

          Dtype input_data = this->blob_bottom_->data_at(i, j, k, l);
          input_mean += input_data;
        }
      }
      sum /= height * width;
      var /= height * width;

      Dtype n = height*width;
      input_mean /= n;

      const Dtype kErrorBound = 0.001;
      // expect zero mean
      EXPECT_NEAR(0, sum, kErrorBound);
      // expect unit variance
      EXPECT_NEAR(1, var, kErrorBound);
      *(expected_input_means.mutable_cpu_data() +
          expected_input_means.offset(i, j, 0, 0)) = input_mean;
    }
  }
}

TYPED_TEST(MVNLayerTest, TestForwardMeanOnly) {
  typedef typename TypeParam::Dtype Dtype;
  LayerParameter layer_param;
  layer_param.ParseFromString("mvn_param{normalize_variance: false}");
  MVNLayer<Dtype> layer(layer_param);
  layer.SetUp(this->blob_bottom_vec_, this->blob_top_vec_, this->blob_finder_);
  layer.Forward(this->blob_bottom_vec_, this->blob_top_vec_);
  // Test mean
  int num = this->blob_bottom_->num();
  int channels = this->blob_bottom_->channels();
  int height = this->blob_bottom_->height();
  int width = this->blob_bottom_->width();

  for (int i = 0; i < num; ++i) {
    for (int j = 0; j < channels; ++j) {
      Dtype sum = 0, var = 0;
      for (int k = 0; k < height; ++k) {
        for (int l = 0; l < width; ++l) {
          Dtype data = this->blob_top_->data_at(i, j, k, l);
          sum += data;
          var += data * data;
        }
      }
      sum /= height * width;

      const Dtype kErrorBound = 0.001;
      // expect zero mean
      EXPECT_NEAR(0, sum, kErrorBound);
    }
  }
}

TYPED_TEST(MVNLayerTest, TestForwardAcrossChannels) {
  typedef typename TypeParam::Dtype Dtype;
  LayerParameter layer_param;
  layer_param.ParseFromString("mvn_param{across_channels: true}");
  MVNLayer<Dtype> layer(layer_param);
  layer.SetUp(this->blob_bottom_vec_, this->blob_top_vec_,
              this->blob_finder_);
  layer.Forward(this->blob_bottom_vec_, this->blob_top_vec_);
  // Test mean
  int num = this->blob_bottom_->num();
  int channels = this->blob_bottom_->channels();
  int height = this->blob_bottom_->height();
  int width = this->blob_bottom_->width();

  for (int i = 0; i < num; ++i) {
    Dtype sum = 0, var = 0;
    for (int j = 0; j < channels; ++j) {
      for (int k = 0; k < height; ++k) {
        for (int l = 0; l < width; ++l) {
          Dtype data = this->blob_top_->data_at(i, j, k, l);
          sum += data;
          var += data * data;
        }
      }
    }
    sum /= height * width * channels;
    var /= height * width * channels;

    const Dtype kErrorBound = 0.001;
    // expect zero mean
    EXPECT_NEAR(0, sum, kErrorBound);
    // expect unit variance
    EXPECT_NEAR(1, var, kErrorBound);
  }
}

TYPED_TEST(MVNLayerTest, TestGradient) {
  typedef typename TypeParam::Dtype Dtype;
  LayerParameter layer_param;
  MVNLayer<Dtype> layer(layer_param);
  GradientChecker<Dtype> checker(1e-2, 1e-3);
  checker.SetBlobFinder(this->blob_finder_);
  checker.CheckGradientExhaustive(&layer, this->blob_bottom_vec_,
      this->blob_top_vec_);
}

TYPED_TEST(MVNLayerTest, TestGradientMeanOnly) {
  typedef typename TypeParam::Dtype Dtype;
  LayerParameter layer_param;
  layer_param.ParseFromString("mvn_param{normalize_variance: false}");
  MVNLayer<Dtype> layer(layer_param);
  GradientChecker<Dtype> checker(1e-2, 1e-3);
  checker.SetBlobFinder(this->blob_finder_);
  checker.CheckGradientExhaustive(&layer, this->blob_bottom_vec_,
      this->blob_top_vec_);
}

TYPED_TEST(MVNLayerTest, TestGradientAcrossChannels) {
  typedef typename TypeParam::Dtype Dtype;
  LayerParameter layer_param;
  layer_param.ParseFromString("mvn_param{across_channels: true}");
  MVNLayer<Dtype> layer(layer_param);
  GradientChecker<Dtype> checker(1e-2, 1e-3);
  checker.SetBlobFinder(this->blob_finder_);
  checker.CheckGradientExhaustive(&layer, this->blob_bottom_vec_,
      this->blob_top_vec_);
}

}  // namespace caffe
