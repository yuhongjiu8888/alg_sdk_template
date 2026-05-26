#include "core/registry/postprocessor_registry.h"
#include "models/ocr_classifier/ocr_classifier_postprocessor.h"

namespace alg {

REGISTER_ALG_POST("ocr_classifier", []{
    return std::unique_ptr<IPostprocessor>(new OcrClassifierPostprocessor());
});

}  // namespace alg
