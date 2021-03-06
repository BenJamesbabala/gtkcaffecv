#include <fstream>
#include <iostream>
#include <cstdlib>

#include "mcvcaffe.h"
#include "mcvwin.h"

#if defined  _OPENCL 
// Get all available GPU devices
static void get_gpus(vector<int>* gpus) {
    int count = 0;

    count = Caffe::EnumerateDevices(true);

    for (int i = 0; i < count; ++i) {
      gpus->push_back(i);
    }
}
#endif 

Classifier::Classifier(const string& model_file,
                       const string& trained_file,
                       const string& mean_file,
                       const string& label_file) {
    
#if defined  _OPENCL 
  // Set device id and mode
  vector<int> gpus;
  get_gpus(&gpus);
  if (gpus.size() != 0) {
    std::cout << "Use GPU with device ID " << gpus[0] << std::endl;
    Caffe::SetDevices(gpus);
    Caffe::set_mode(Caffe::GPU);
    Caffe::SetDevice(gpus[0]);
  }
#else 
  std::cout << "Use CPU" << std::endl;
  Caffe::set_mode(Caffe::CPU);
#endif
  
  /* Load the network. */
#if defined  _OPENCL 
  net_.reset(new Net<float>(model_file, TEST, Caffe::GetDefaultDevice()));
#else
  net_.reset(new Net<float>(model_file, TEST));
#endif
  
  net_->CopyTrainedLayersFrom(trained_file);

  CHECK_EQ(net_->num_inputs(), 1) << "Network should have exactly one input.";
  CHECK_EQ(net_->num_outputs(), 1) << "Network should have exactly one output.";

  Blob<float>* input_layer = net_->input_blobs()[0];
  num_channels_ = input_layer->shape(1);
  CHECK(num_channels_ == 3 || num_channels_ == 1)
    << "Input layer should have 1 or 3 channels.";
  input_geometry_ = cv::Size(input_layer->width(), input_layer->height());

  /* Load the binaryproto mean file. */
  SetMean(mean_file);

  /* Load labels. */
  std::ifstream labels(label_file.c_str());
  CHECK(labels) << "Unable to open labels file " << label_file;
  string line;
  while (std::getline(labels, line))
    labels_.push_back(string(line));

  Blob<float>* output_layer = net_->output_blobs()[0];
  CHECK_EQ(labels_.size(), output_layer->shape(1))
    << "Number of labels is different from the output layer dimension.";
}

static bool PairCompare(const std::pair<float, int_tp>& lhs,
                        const std::pair<float, int_tp>& rhs) {
  return lhs.first > rhs.first;
}

/* Return the indices of the top N values of vector v. */
static std::vector<int_tp> Argmax(const std::vector<float>& v, int_tp N) {
  std::vector<std::pair<float, int_tp> > pairs;
  for (uint_tp i = 0; i < v.size(); ++i)
    pairs.push_back(std::make_pair(v[i], i));
  std::partial_sort(pairs.begin(), pairs.begin() + N, pairs.end(), PairCompare);

  std::vector<int_tp> result;
  for (int_tp i = 0; i < N; ++i)
    result.push_back(pairs[i].second);
  return result;
}

/* Return the top N predictions. */
std::vector<Prediction> Classifier::Classify(const cv::Mat& img, int_tp N) {
  std::vector<float> output = Predict(img);

  N = std::min<int_tp>(labels_.size(), N);
  std::vector<int_tp> maxN = Argmax(output, N);
  std::vector<Prediction> predictions;
  for (int_tp i = 0; i < N; ++i) {
    int_tp idx = maxN[i];
    predictions.push_back(std::make_pair(labels_[idx], output[idx]));
  }

  return predictions;
}

/* Load the mean file in binaryproto format. */
void Classifier::SetMean(const string& mean_file) {
  BlobProto blob_proto;
  ReadProtoFromBinaryFileOrDie(mean_file.c_str(), &blob_proto);

  /* Convert from BlobProto to Blob<float> */
  Blob<float> mean_blob;
  mean_blob.FromProto(blob_proto);
  CHECK_EQ(mean_blob.shape(1), num_channels_)
    << "Number of channels of mean file doesn't match input layer.";

  /* The format of the mean file is planar 32-bit float BGR or grayscale. */
  std::vector<cv::Mat> channels;
  float* data = mean_blob.mutable_cpu_data();
  for (int_tp i = 0; i < num_channels_; ++i) {
    /* Extract an individual channel. */
    cv::Mat channel(mean_blob.height(), mean_blob.width(), CV_32FC1, data);
    channels.push_back(channel);
    data += mean_blob.height() * mean_blob.width();
  }

  /* Merge the separate channels into a single image. */
  cv::Mat mean;
  cv::merge(channels, mean);

  /* Compute the global mean pixel value and create a mean image
   * filled with this value. */
  cv::Scalar channel_mean = cv::mean(mean);
  mean_ = cv::Mat(input_geometry_, mean.type(), channel_mean);
}

std::vector<float> Classifier::Predict(const cv::Mat& img) {
  Blob<float>* input_layer = net_->input_blobs()[0];
  input_layer->Reshape(1, num_channels_,
                       input_geometry_.height, input_geometry_.width);
  /* Forward dimension change to all layers. */
  net_->Reshape();

  std::vector<cv::Mat> input_channels;
  WrapInputLayer(&input_channels);

  Preprocess(img, &input_channels);
  
#if defined  _OPENCL 
  net_->Forward();
#else 
  net_->ForwardPrefilled();
#endif

  /* Copy the output layer to a std::vector */
  Blob<float>* output_layer = net_->output_blobs()[0];
  const float* begin = output_layer->cpu_data();
  const float* end = begin + output_layer->shape(1);
  return std::vector<float>(begin, end);
}

/* Wrap the input layer of the network in separate cv::Mat objects
 * (one per channel). This way we save one memcpy operation and we
 * don't need to rely on cudaMemcpy2D. The last preprocessing
 * operation will write the separate channels directly to the input
 * layer. */
void Classifier::WrapInputLayer(std::vector<cv::Mat>* input_channels) {
  Blob<float>* input_layer = net_->input_blobs()[0];

  int_tp width = input_layer->width();
  int_tp height = input_layer->height();
  float* input_data = input_layer->mutable_cpu_data();
  for (int_tp i = 0; i < input_layer->shape(1); ++i) {
    cv::Mat channel(height, width, CV_32FC1, input_data);
    input_channels->push_back(channel);
    input_data += width * height;
  }
}

void Classifier::Preprocess(const cv::Mat& img,
                            std::vector<cv::Mat>* input_channels) {
  /* Convert the input image to the input image format of the network. */
  cv::Mat sample;
  if (img.channels()== 3 && num_channels_ == 1)
    cv::cvtColor(img, sample, cv::COLOR_BGR2GRAY);
  else if (img.channels() == 4 && num_channels_ == 1)
    cv::cvtColor(img, sample, cv::COLOR_BGRA2GRAY);
  else if (img.channels() == 4 && num_channels_ == 3)
    cv::cvtColor(img, sample, cv::COLOR_BGRA2BGR);
  else if (img.channels() == 1 && num_channels_ == 3)
    cv::cvtColor(img, sample, cv::COLOR_GRAY2BGR);
  else
    sample = img;

  cv::Mat sample_resized;
  if (sample.size() != input_geometry_)
    cv::resize(sample, sample_resized, input_geometry_);
  else
    sample_resized = sample;

  cv::Mat sample_float;
  if (num_channels_ == 3)
    sample_resized.convertTo(sample_float, CV_32FC3);
  else
    sample_resized.convertTo(sample_float, CV_32FC1);

  cv::Mat sample_normalized;
  cv::subtract(sample_float, mean_, sample_normalized);

  /* This operation will write the separate BGR planes directly to the
   * input layer of the network because it is wrapped by the cv::Mat
   * objects in input_channels. */
  cv::split(sample_normalized, *input_channels);

  CHECK(reinterpret_cast<float*>(input_channels->at(0).data)
        == net_->input_blobs()[0]->cpu_data())
    << "Input channels are not wrapping the input layer of the network.";
}


Classifier::~Classifier()
{
    std::cout << "~Classifier" << std::endl;
}

void  Classifier::signal_connect(McvWin  *p_win)
{
    signal_thread.connect(sigc::bind<1>(sigc::mem_fun(*p_win, &McvWin::on_classifier_signal), this));
}

void  Classifier::load_image(cv::Mat &img)
{
    m_img = img.clone();
}

void  Classifier::fun()
{
    m_predictions.clear();

    double time_b = (double)getTickCount();

    m_predictions = Classify(m_img);

    double time_e = (double)getTickCount();
    double time_nn = (time_e - time_b)/getTickFrequency()*1000.0;

    std::ostringstream    os_str;

    os_str << "Caffe time : " << time_nn << " ms" << std::endl;

    /* Print the top N predictions. */
    for (size_t i = 0; i < m_predictions.size(); ++i)
    {
        Prediction p = m_predictions[i];
        os_str << std::fixed << std::setprecision(4) << p.second << " - \"" << p.first << "\"" << std::endl;
    }

    std::unique_lock<std::mutex> lock(m_mutex);

    m_str_class = os_str.str();

    signal_thread();
}

void  Classifier::get_str_class(std::string  &str_class)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    str_class = m_str_class;

    lock.unlock();

    mptr_thread.release();
}
