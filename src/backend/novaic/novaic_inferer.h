/** NT98332 HDAL/GFX and AI2 inference backend. */
#ifndef ALG_BACKEND_NOVAIC_NOVAIC_INFERER_H
#define ALG_BACKEND_NOVAIC_NOVAIC_INFERER_H

#include <memory>
#include "core/image_view.h"
#include "core/infer/inferer.h"
#include "core/solution/crop_geometry.h"
extern "C" {
#include "hdal.h"
#include "vendor_ai.h"
}

namespace alg {

class NovaicInferer : public IInferer {
  public:
    ~NovaicInferer() override;
    Status Load(const std::string& path) override;
    Status Forward() override;
    Status ValidatePreprocessConfig(const PreprocessConfig& cfg) const override;
    bool SupportsHardwarePreprocess() const override { return true; }
    bool SupportsNv12Roi() const override { return true; }
    Status PrepareHardwareInput(const AlgImage&,
                                const PreprocessConfig&,
                                PreprocessState&,
                                SharedInputStaging*) override;
    Status PrepareNv12Roi(const AlgImage&,
                          const CropGeometry&,
                          const CropConfig&,
                          const PreprocessConfig&,
                          PreprocessState&,
                          CropTransform*,
                          SharedInputStaging*) override;

  private:
    struct Runtime;
    struct Buffer {
        UINTPTR pa = 0;
        void* va = nullptr;
        UINT32 size = 0;
        void Allocate(const char* name, uint64_t bytes);
        void Free();
    };
    struct Image {
        Buffer buffer;
        HD_GFX_IMG_BUF gfx{};
        void Allocate(const char* name, int width, int height);
        ~Image() { buffer.Free(); }
    };

    void FreeAll();
    Status Prepare(const AlgImage&,
                   const CropGeometry&,
                   int crop_pad,
                   const PreprocessConfig&,
                   PreprocessState&,
                   SharedInputStaging*);
    HD_GFX_IMG_BUF Upload(const AlgImage&, const ImageView&, SharedInputStaging*);
    void ReadOutput(size_t index, bool copy);
    std::shared_ptr<Runtime> runtime_;
    UINT32 id_ = 0, input_path_ = 0;
    bool acquired_ = false, opened_ = false, started_ = false, prepared_ = false;
    Buffer model_, work_;
    Image canvas_;
    std::unique_ptr<Image> source_;
    VENDOR_AI_BUF input_{};
    std::vector<UINT32> output_paths_;
    std::vector<std::vector<int16_t>> outputs_;
};

}  // namespace alg

#endif  // ALG_BACKEND_NOVAIC_NOVAIC_INFERER_H
