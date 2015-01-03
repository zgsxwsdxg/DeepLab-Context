#include <algorithm>
#include <vector>

#include "caffe/layer.hpp"
#include "caffe/vision_layers.hpp"
#include "caffe/util/densecrf_util.hpp"
#include "caffe/util/densecrf_pairwise.hpp"
#include "caffe/util/math_functions.hpp"

// TODO: Add SemiMetricFunction


namespace caffe {

template <typename Dtype>
void DenseCRFLayer<Dtype>::LayerSetUp(
  const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {

  DenseCRFParameter dense_crf_param = this->layer_param_.dense_crf_param();

  max_iter_ = dense_crf_param.max_iter();
  
  if (dense_crf_param.pos_w_size() > 0) {
    for (int i = 0; i < dense_crf_param.pos_w_size(); ++i) {
      pos_w_.push_back(dense_crf_param.pos_w(i));
    }
  }

  if (dense_crf_param.pos_xy_std_size() > 0) {
    for (int i = 0; i < dense_crf_param.pos_xy_std_size(); ++i) {
      pos_xy_std_.push_back(dense_crf_param.pos_xy_std(i));
    }
  }

  if (dense_crf_param.bi_w_size() > 0) {
    for (int i = 0; i < dense_crf_param.bi_w_size(); ++i) {
      bi_w_.push_back(dense_crf_param.bi_w(i));
    }
  }

  if (dense_crf_param.bi_xy_std_size() > 0) {
    for (int i = 0; i < dense_crf_param.bi_xy_std_size(); ++i) {
      bi_xy_std_.push_back(dense_crf_param.bi_xy_std(i));
    }
  }

  if (dense_crf_param.bi_rgb_std_size() > 0) {
    for (int i = 0; i < dense_crf_param.bi_rgb_std_size(); ++i) {
      bi_rgb_std_.push_back(dense_crf_param.bi_rgb_std(i));
    }
  }

  CHECK_EQ(pos_w_.size(), pos_xy_std_.size())
    << "pos_w and pos_xy_std should have the same size.";
  CHECK_EQ(bi_w_.size(), bi_xy_std_.size())
    << "bi_w and bi_xy_std should have the same size.";
  CHECK_EQ(bi_w_.size(), bi_rgb_std_.size())
    << "bi_w and bi_rgb_std should have the same size.";
    
  CHECK_GE(bottom.size(), 2) 
    << "bottom must have size larger than 2 (i.e., DCNN output and image dim).";

  if (bottom.size() <= 2) {
    has_image = false;
  } else {
    has_image = true;
    CHECK_GT(bi_w_.size(), 0)
      << "has image as input, but no bilateral parameters specified.";
    CHECK_EQ(bottom[2]->channels(), 3)
      << "Can Only support color images for now.";
  }

  unary_element_ = 0;
  map_element_   = 0;
  unary_   = NULL;
  current_ = NULL;
  next_    = NULL;
  tmp_     = NULL;

}

template <typename Dtype>
void DenseCRFLayer<Dtype>::Reshape(
  const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  // assume bottom[0]: output from DCNN (after upsampling)
  //        bottom[1]: dimension for each image (i.e., store effective dimensions)
  //        bottom[2]: images after data-transformer (optional, if no bilateral)
  //        top[0]   : inference values
  //        top[1]   : map results
  //

  num_ = bottom[0]->num();
  M_   = bottom[0]->channels();
  pad_height_   = bottom[0]->height();
  pad_width_    = bottom[0]->width();

  CHECK_EQ(bottom[0]->num(), bottom[1]->num())
    << "The DCNN output and data should have the same number.";
  CHECK_EQ(bottom[1]->num(), bottom[2]->num())
    << "The data and data dimension should have the same number.";

  if (has_image) {
    CHECK_EQ(bottom[0]->height(), bottom[2]->height())
      << "DCNN output after upsampling should have the same height as image.";
    CHECK_EQ(bottom[0]->width(), bottom[2]->width())
      << "DCNN output after upsampling should have the same width as image.";
  }

  int num_pixel  = pad_height_ * pad_width_;
  int cur_unary_element = num_pixel * M_;

  if (unary_element_ < cur_unary_element) {
    unary_element_ = cur_unary_element;
    map_element_   = num_pixel;
    
    // allocate largest possible size for data arrays
    DeAllocateAllData();
    AllocateAllData();
  }

  // allocate largest possible size for top
  top[0]->Reshape(num_, M_, pad_height_, pad_width_);
  top[1]->Reshape(num_,  1, pad_height_, pad_width_);

  sum_multiplier_.Reshape(1, M_, 1, 1);
  Dtype* multiplier_data = sum_multiplier_.mutable_cpu_data();
  for (int i = 0; i < sum_multiplier_.count(); ++i) {
    multiplier_data[i] = 1.;
  }
  scale_.Reshape(1, 1, pad_height_, pad_width_);
  norm_data_.Reshape(1, M_, pad_height_, pad_width_);
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  // assume bottom[0]: output from DCNN
  //        bottom[1]: dimension for each image
  //        bottom[2]: images after data-transformer (optional, if no bilateral)
  //        top[0]   : inference values
  //        top[1]   : map results
  //

  const Dtype* bottom_data = bottom[0]->cpu_data();
  const Dtype* data_dims   = bottom[1]->cpu_data();

  int data_offset;
  int data_dim_offset;

  for (int n = 0; n < num_; ++n) {
    data_offset     = bottom[0]->offset(n);
    data_dim_offset = bottom[1]->offset(n);

    // check dimension of data arrays
    // if too small, reallocate memory
    int real_img_height = *(data_dims + data_dim_offset);
    int real_img_width  = *(data_dims + data_dim_offset + 1);     
    // Get N, W, H, M
    if (pad_height_ <= real_img_height && pad_width_ <= real_img_width) {
      // image may be cropped
      H_ = pad_height_;
      W_ = pad_width_;
      N_ = W_ * H_;
    } else {
      // image is padded with redudant values
      H_ = real_img_height;
      W_ = real_img_width;
      N_ = W_ * H_;
    }

    // check if the pre-allocated memory is not enough
    CHECK_LE(N_, map_element_)
      << "The pre-allocated memory is not enough!";

    /*
    if (N_ > map_element_) {
      DeAllocateAllData();
      map_element_   = N_;
      unary_element_ = N_ * M_;
      AllocateAllData();
    }
    */

    SetupUnaryEnergy(bottom_data + data_offset);
    SetupPairwiseFunctions(bottom);
    ComputeMap(n, top);
    ClearPairwiseFunctions();
  }
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  NOT_IMPLEMENTED;
}

template <typename Dtype>
DenseCRFLayer<Dtype>::~DenseCRFLayer() {
    ClearPairwiseFunctions();
    DeAllocateAllData();
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::DeAllocateAllData() {
    deallocate(unary_);
    deallocate(current_);
    deallocate(next_);
    deallocate(tmp_);
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::AllocateAllData() {
  unary_   = allocate(unary_element_);
  current_ = allocate(unary_element_);
  next_    = allocate(unary_element_);
  tmp_     = allocate(unary_element_);

}

template <typename Dtype>
void DenseCRFLayer<Dtype>::ExpAndNormalize(float* out, const float* in, float scale) {
  float* V = new float[M_];

  for (int i = 0; i < N_; ++i) {
    const float* b = in + i*M_;
    // Find the max and subtract it so that the exp doesn't explode
    float mx = scale*b[0];
    for (int j = 1; j < M_; ++j)
      if( mx < scale*b[j] )
	mx = scale*b[j];
    float tt = 0;
    for (int j = 0; j < M_; ++j) {
      V[j] = fast_exp(scale*b[j]-mx);
      tt += V[j];
    }
    // Make it a probability
    for (int j=0; j < M_; ++j)
      V[j] /= tt;
		
    float* a = out + i*M_;
    for (int j = 0; j < M_; ++j)
      a[j] = V[j];
  }
  delete[] V;
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::StartInference() {
  ExpAndNormalize(current_, unary_, -1.0);
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::StepInference() {
#ifdef SSE_DENSE_CRF
  __m128 * sse_next_ = (__m128*)next_;
  __m128 * sse_unary_ = (__m128*)unary_;
#endif
  // Set the unary potential
#ifdef SSE_DENSE_CRF
  for (int i = 0; i < (N_*M_-1)/4+1; ++i)
    sse_next_[i] = - sse_unary_[i];
#else
  for (int i = 0; i < N_*M_; ++i)
    next_[i] = -unary_[i];
#endif
	
  // Add up all pairwise potentials
  for (size_t i=0; i < pairwise_.size(); ++i)
    pairwise_[i]->apply(next_, current_, tmp_, M_);
	
  // Exponentiate and normalize
  ExpAndNormalize(current_, next_, 1.0);
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::ClearPairwiseFunctions() {
  for (size_t i = 0; i < pairwise_.size(); ++i) {
    delete pairwise_[i];
  }
  pairwise_.clear();
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::RunInference() {
  StartInference();

  for (int i = 0; i < max_iter_; ++i) {
    StepInference();
  }
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::ComputeMap(const int num, const vector<Blob<Dtype>*>& top) {
  // compute map for num-th data
  //

  Dtype* top_inf = top[0]->mutable_cpu_data() + top[0]->offset(num);
  Dtype* top_map = top[1]->mutable_cpu_data() + top[1]->offset(num);

  int top_channels = top[0]->channels();
  int top_height   = top[0]->height();
  int top_width    = top[0]->width();
  memset(top_inf, 0, sizeof(Dtype)*top_channels*top_height*top_width);
  memset(top_map, 0, sizeof(Dtype)*top_height*top_width);

  CHECK_EQ(top_channels, M_);
  CHECK_EQ(top_height, pad_height_);
  CHECK_EQ(top_width, pad_width_);

  // results are saved to current_, after call RunInference()
  RunInference();

  int in_index;
  int out_index;

  for (int h = 0; h < H_; ++h) {
    for (int w = 0; w < W_; ++w) {      
      // c = 0
      in_index  = (h * W_ + w) * M_;
      out_index = h * top_width + w;
      top_inf[out_index] = static_cast<Dtype>(current_[in_index]);

      // find the map
      float mx = top_inf[out_index];
      int imx = 0;

      for (int c = 1; c < M_; ++c) {
	in_index  = (h * W_ + w) * M_ + c;
	out_index = (c * top_height + h) * top_width + w;

	top_inf[out_index] = static_cast<Dtype>(current_[in_index]);

	if (mx < top_inf[out_index]) {
	  mx = top_inf[out_index];
	  imx = c;
	}
      }

      out_index = h * top_width + w;
      // copy to top[1]. assume row-order
      top_map[out_index] = static_cast<Dtype>(imx);
    } 
  }
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::SetupPairwiseFunctions(const vector<Blob<Dtype>*>& bottom) {
  ClearPairwiseFunctions();

  // add pairwise Gaussian
  for (size_t k = 0; k < pos_w_.size(); ++k) {
    float* features = new float[N_*2];
    for (int j = 0; j < H_; ++j) {
      for (int i = 0; i < W_; ++i) {
	features[(j*W_+i)*2+0] = i / pos_xy_std_[k];
	features[(j*W_+i)*2+1] = j / pos_xy_std_[k];
      }
    }
    pairwise_.push_back(new PottsPotential(features, 2, N_, pos_w_[k]));
    delete[] features;
  }

  // read image
  if (has_image) {
    const Dtype* im = bottom[2]->cpu_data();
    int channel_offset = pad_height_ * pad_width_;

    // add pairwise Bilateral
    for (size_t k = 0; k < bi_w_.size(); ++k) {
      float* features = new float[N_*5];

      // Note H_ and W_ are the effective dimension of image (not padded dimensions)
      for (int j = 0; j < H_; j++) {
	for (int i = 0; i < W_; i++){
	  features[(j*W_+i)*5+0] = i / bi_xy_std_[k];
	  features[(j*W_+i)*5+1] = j / bi_xy_std_[k];

	  int img_index = j * pad_width_ + i;

	  // im is BGR
	  // Assume im is mean-centered (not affect gaussian blur)
	  // and assume im is proprocessing by scale = 1 (may cause problem if not 1)
	  features[(j*W_+i)*5+2] = im[img_index] / bi_rgb_std_[k];
	  features[(j*W_+i)*5+3] = im[img_index + channel_offset] / bi_rgb_std_[k];
	  features[(j*W_+i)*5+4] = im[img_index + 2*channel_offset] / bi_rgb_std_[k];
	}
      }
      pairwise_.push_back(new PottsPotential(features, 5, N_, bi_w_[k]));
      delete[] features;
    }
  }
}

template <typename Dtype>
void DenseCRFLayer<Dtype>::SetupUnaryEnergy(const Dtype* bottom_data) {
  // take exp and then -log
  Dtype* scale_data = scale_.mutable_cpu_data();
  Dtype* norm_data  = norm_data_.mutable_cpu_data();
  int spatial_dim = pad_height_ * pad_width_;

  // norm_data is the normalized result of bottom_data
  caffe_copy(spatial_dim * M_, bottom_data, norm_data);
  // initialize scale_data to the first plane
  caffe_copy(spatial_dim, bottom_data, scale_data);

  // subtract the max to avoid numerical issues, compute the exp,
  // and then normalize.

  // get max
  for (int j = 1; j < M_; ++j) {
    for (int k = 0; k < spatial_dim; ++k) {
      scale_data[k] = std::max(scale_data[k],
       bottom_data[j * spatial_dim + k]);
    }
  }

  // subtraction
  caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, M_, spatial_dim, 
      1, -1., sum_multiplier_.cpu_data(), scale_data, 1., norm_data);

  // exponentiation
  caffe_exp<Dtype>(spatial_dim * M_, norm_data, norm_data);

  // sum after exp
  caffe_cpu_gemv<Dtype>(CblasTrans, M_, spatial_dim, 1., norm_data, 
			sum_multiplier_.cpu_data(), 0., scale_data);

  // division
  for (int j = 0; j < M_; ++j) {
    caffe_div(spatial_dim, norm_data + j * spatial_dim, scale_data, 
	      norm_data + j * spatial_dim);
  }

  // crop the effective size to unary_ and take -log
  for (int c = 0; c < M_; ++c) {
    for (int h = 0; h < H_; ++h) {
      for (int w = 0; w < W_; ++w) {
	int in_index  = (c * pad_height_ + h) * pad_width_ + w;
	int out_index = (h * pad_width_ + w) * M_ + c;
	unary_[out_index] = -log(norm_data[in_index]);
      }
    }
  }

}

  
INSTANTIATE_CLASS(DenseCRFLayer);
REGISTER_LAYER_CLASS(DENSE_CRF, DenseCRFLayer);

}  // namespace caffe
