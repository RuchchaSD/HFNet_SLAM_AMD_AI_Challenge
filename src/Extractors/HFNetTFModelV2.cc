#include "Extractors/HFNetTFModelV2.h"

#ifdef USE_TENSORFLOW

using namespace cv;
using namespace std;
using namespace tensorflow;

namespace ORB_SLAM3
{

HFNetTFModelV2::HFNetTFModelV2(const std::string &strModelDir, ModelDetectionMode mode, const cv::Vec4i inputShape)
{
    mbVaild = LoadHFNetTFModel(strModelDir);

    if (!mbVaild) return;

    mMode = mode;
    mInputShape = {inputShape(0), inputShape(1), inputShape(2), inputShape(3)};
    if (mMode == kImageToLocalAndGlobal)
    {
        mvInputTensors.emplace_back("image", Tensor(DT_FLOAT, mInputShape));

        mvOutputTensorNames.emplace_back("scores_dense_nms");
        mvOutputTensorNames.emplace_back("local_descriptor_map");
        mvOutputTensorNames.emplace_back("global_descriptor");
    }
    else if (mMode == kImageToLocal)
    {
        mvInputTensors.emplace_back("image", Tensor(DT_FLOAT, mInputShape));

        mvOutputTensorNames.emplace_back("scores_dense_nms");
        mvOutputTensorNames.emplace_back("local_descriptor_map");
    }
    else if (mMode == kImageToLocalAndIntermediate)
    {
        mvInputTensors.emplace_back("image", Tensor(DT_FLOAT, mInputShape));

        mvOutputTensorNames.emplace_back("scores_dense_nms");
        mvOutputTensorNames.emplace_back("local_descriptor_map");
        mvOutputTensorNames.emplace_back("pred/MobilenetV2/expanded_conv_6/input:0");
    }
    else if (mMode == kIntermediateToGlobal)
    {
        mvInputTensors.emplace_back("pred/MobilenetV2/expanded_conv_6/input:0", Tensor(DT_FLOAT, mInputShape));

        mvOutputTensorNames.emplace_back("global_descriptor");
    }
    else
    {
        mbVaild = false;
        return;
    }
    
    // The tensorflow model cost huge time at the first detection.
    // Therefore, give a fake data to compile
    // The size of fake image should be the same as the real image.
    std::vector<tensorflow::Tensor> vNetResults;
    mbVaild = Run(vNetResults);
}

bool HFNetTFModelV2::Detect(const cv::Mat &image, std::vector<cv::KeyPoint> &vKeyPoints, cv::Mat &localDescriptors, cv::Mat &globalDescriptors,
                            int nKeypointsNum, float threshold)
{
    if (mMode != kImageToLocalAndGlobal && mMode != kImageToLocalAndIntermediate) return false;

    Mat2Tensor(image, &mvInputTensors[0].second);

    if (!Run(mvNetResults)) return false;

    if (mMode == kImageToLocalAndGlobal)
        GetGlobalDescriptorFromTensor(mvNetResults[2], globalDescriptors);
    else Tensor2Mat(&mvNetResults[2], globalDescriptors);
    GetLocalFeaturesFromTensor(mvNetResults[0], mvNetResults[1], vKeyPoints, localDescriptors, nKeypointsNum, threshold);
    return true;
}

bool HFNetTFModelV2::Detect(const cv::Mat &image, std::vector<cv::KeyPoint> &vKeyPoints, cv::Mat &localDescriptors,
                            int nKeypointsNum, float threshold)
{
    if (mMode != kImageToLocal) return false;

    Mat2Tensor(image, &mvInputTensors[0].second);

    if (!Run(mvNetResults)) return false;
    GetLocalFeaturesFromTensor(mvNetResults[0], mvNetResults[1], vKeyPoints, localDescriptors, nKeypointsNum, threshold);
    return true;
}

bool HFNetTFModelV2::Detect(const cv::Mat &intermediate, cv::Mat &globalDescriptors)
{
    if (mMode != kIntermediateToGlobal) return false;

    Mat2Tensor(intermediate, &mvInputTensors[0].second);
    if (!Run(mvNetResults)) return false;
    GetGlobalDescriptorFromTensor(mvNetResults[0], globalDescriptors);
    return true;
}

bool HFNetTFModelV2::Run(std::vector<tensorflow::Tensor> &vNetResults) 
{
    if (!mbVaild) return false;
    if (mvInputTensors.empty() || mvInputTensors[0].second.shape() != mInputShape) return false;

    Status status = mSession->Run(mvInputTensors, mvOutputTensorNames, {}, &vNetResults);

    if (!status.ok()) cerr << status.error_message() << endl;
    return status.ok();
}

void HFNetTFModelV2::GetLocalFeaturesFromTensor(const tensorflow::Tensor &tScoreDense, const tensorflow::Tensor &tDescriptorsMap,
                                                std::vector<cv::KeyPoint> &vKeyPoints, cv::Mat &localDescriptors, 
                                                int nKeypointsNum, float threshold)
{
    auto vResScoresDense = tScoreDense.tensor<float, 3>(); // shape: [1 image.height image.width]
    auto vResLocalDescriptorMap = tDescriptorsMap.tensor<float, 4>();

    const int width = tScoreDense.shape().dim_size(2), height = tScoreDense.shape().dim_size(1);
    const float scaleWidth = (tDescriptorsMap.shape().dim_size(2) - 1.f) / (float)(tScoreDense.shape().dim_size(2) - 1.f);
    const float scaleHeight = (tDescriptorsMap.shape().dim_size(1) - 1.f) / (float)(tScoreDense.shape().dim_size(1) - 1.f);

    cv::KeyPoint keypoint;
    keypoint.angle = 0;
    keypoint.octave = 0;
    vKeyPoints.clear();
    vKeyPoints.reserve(2 * nKeypointsNum);
    for (int col = 0; col < width; ++col)
    {
        for (int row = 0; row < height; ++row)
        {
            float score = vResScoresDense(row * width + col);
            if (score >= threshold)
            {
                keypoint.pt.x = col;
                keypoint.pt.y = row;
                keypoint.response = score;
                vKeyPoints.emplace_back(keypoint);
            }
        }
    }

    // vKeyPoints = NMS(vKeyPoints, width, height, 4);

    if (vKeyPoints.size() > nKeypointsNum)
    {
        // vKeyPoints = DistributeOctTree(vKeyPoints, 0, width, 0, height, nKeypointsNum);
        std::nth_element(vKeyPoints.begin(), vKeyPoints.begin() + nKeypointsNum, vKeyPoints.end(), [](const cv::KeyPoint& p1, const cv::KeyPoint& p2) {
            return p1.response > p2.response;
        });
        vKeyPoints.erase(vKeyPoints.begin() + nKeypointsNum, vKeyPoints.end());
    }
    
    localDescriptors = cv::Mat(vKeyPoints.size(), 256, CV_32F);
    Tensor tWarp(DT_FLOAT, TensorShape({(int)vKeyPoints.size(), 2}));
    auto pWarp = tWarp.tensor<float, 2>();
    for (size_t temp = 0; temp < vKeyPoints.size(); ++temp)
    {
        pWarp(temp * 2 + 0) = scaleWidth * vKeyPoints[temp].pt.x;
        pWarp(temp * 2 + 1) = scaleHeight * vKeyPoints[temp].pt.y;
    }

    ResamplerTF(tDescriptorsMap, tWarp, localDescriptors);

    for (int index = 0; index < localDescriptors.rows; ++index)
    {
        cv::normalize(localDescriptors.row(index), localDescriptors.row(index));
    }
}

void HFNetTFModelV2::GetGlobalDescriptorFromTensor(const tensorflow::Tensor &tDescriptors, cv::Mat &globalDescriptors)
{
    auto vResGlobalDescriptor = tDescriptors.tensor<float, 2>();
    globalDescriptors = cv::Mat(4096, 1, CV_32F);
    for (int temp = 0; temp < 4096; ++temp)
    {
        globalDescriptors.ptr<float>(0)[temp] = vResGlobalDescriptor(temp);
    }
}

bool HFNetTFModelV2::LoadHFNetTFModel(const std::string &strModelDir)
{
    mstrTFModelPath = strModelDir;
    tensorflow::Status status;
    tensorflow::SessionOptions sessionOptions;
    tensorflow::RunOptions runOptions;
    tensorflow::SavedModelBundle bundle;

    status = LoadSavedModel(sessionOptions, runOptions, strModelDir, {tensorflow::kSavedModelTagServe}, &bundle);
    if(!status.ok()){
        std::cerr << "Failed to load HFNet model at path: " << strModelDir <<std::endl;
        return false;
    }

    mSession = std::move(bundle.session);
    status = mSession->Create(mGraph);
    if(!status.ok()){
        std::cerr << "Failed to create mGraph for HFNet" << std::endl;
        return false;
    }

    return true;
}

void HFNetTFModelV2::Mat2Tensor(const cv::Mat &mat, tensorflow::Tensor *tensor)
{
    cv::Mat fromMat(mat.rows, mat.cols, CV_32FC(mat.channels()), tensor->flat<float>().data());
    mat.convertTo(fromMat, CV_32F);
}

void HFNetTFModelV2::Tensor2Mat(tensorflow::Tensor *tensor, cv::Mat &mat)
{
    const cv::Mat fromTensor(cv::Size(tensor->shape().dim_size(1), tensor->shape().dim_size(2)), CV_32FC(tensor->shape().dim_size(3)), tensor->flat<float>().data());
    fromTensor.convertTo(mat, CV_32F);
}

void HFNetTFModelV2::ResamplerTF(const tensorflow::Tensor &data, const tensorflow::Tensor &warp, cv::Mat &output)
{
    const tensorflow::TensorShape& data_shape = data.shape();
    const int batch_size = data_shape.dim_size(0);
    const int data_height = data_shape.dim_size(1);
    const int data_width = data_shape.dim_size(2);
    const int data_channels = data_shape.dim_size(3);
    const tensorflow::TensorShape& warp_shape = warp.shape();

    tensorflow::TensorShape output_shape = warp.shape();
    // output_shape.set_dim(output_shape.dims() - 1, data_channels);
    // output = Tensor(DT_FLOAT, output_shape);
    output = cv::Mat(output_shape.dim_size(0), data_channels, CV_32F);
    
    const int num_sampling_points = warp.NumElements() / batch_size / 2;
    if (num_sampling_points > 0)
    {
        Resampler(data.flat<float>().data(), warp.flat<float>().data(), output.ptr<float>(),
                  batch_size, data_height, data_width, 
                  data_channels, num_sampling_points);
    }
}

} // namespace ORB_SLAM3

#endif // USE_TENSORFLOW