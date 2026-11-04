/** EEasyTech 826 / JC371 OpenVX backend. */
#ifndef ALG_BACKEND_EZ_EZ_INFERER_H
#define ALG_BACKEND_EZ_EZ_INFERER_H

#include <string>
#include <vector>
#include <VX/vx.h>
#include <net_api.h>
#include "core/image_view.h"
#include "core/infer/inferer.h"
#include "core/solution/crop_geometry.h"

namespace alg {

class EzInferer : public IInferer {
  public:
    ~EzInferer() override;
    Status Load(const std::string& model_path) override;
    Status Forward() override;
    Status ValidatePreprocessConfig(const PreprocessConfig&) const override;
    bool SupportsHardwarePreprocess() const override { return true; }
    bool SupportsNv12Roi() const override { return true; }
    Status PrepareHardwareInput(const AlgImage&, const PreprocessConfig&, PreprocessState&, SharedInputStaging*) override;
    Status PrepareNv12Roi(const AlgImage&, const AlgBox&, const CropConfig&, const PreprocessConfig&,
                          PreprocessState&, CropTransform*, SharedInputStaging*) override;

  private:
    struct DeviceImage {
        vx_size handle = 0;
        vx_size physical = 0;
        uint8_t* data = nullptr;
        size_t size = 0;
        int width = 0;
        int height = 0;
        img_fmt format = BGRA_WHC;
    };

    void FreeAll();
    void FreeImage(DeviceImage*);
    Status EnsureImage(DeviceImage*, img_fmt, int, int);
    Status BuildViews();
    Status ValidateConfig(const AlgImage&, const PreprocessConfig&) const;
    Status BindSource(const AlgImage&, const ImageView&, vx_size*, vx_size*, int*, bool*, bool*, SharedInputStaging*);
    Status Prepare(const AlgImage&, const PreprocessConfig&, const CropGeometry&, int, PreprocessState&,
                   SharedInputStaging*);

    vx_context context_ = nullptr;
    vx_graph graph_ = nullptr;
    bool kernels_loaded_ = false;
    bool initialized_ = false;
    bool prepared_ = false;
    int net_width_ = 0;
    int net_height_ = 0;
    std::vector<std::string> input_names_;
    std::vector<std::string> output_names_;
    // Graph owns the tensors returned by GetNet{Input,Output}Blob, as in the samples.
    std::vector<vx_tensor> input_tensors_;
    std::vector<vx_tensor> output_tensors_;
    std::vector<net_blob_data> exported_outputs_;
    std::vector<std::vector<float>> output_placeholders_;
    DeviceImage source_;
    DeviceImage canvas_;
    DeviceImage scratch_;
};

}  // namespace alg

#endif
