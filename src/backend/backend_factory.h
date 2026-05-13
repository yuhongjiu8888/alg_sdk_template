/**
 * @file backend_factory.h
 * @brief One symbol per backend; CMake compiles exactly one of them.
 */

#ifndef ALG_BACKEND_BACKEND_FACTORY_H
#define ALG_BACKEND_BACKEND_FACTORY_H

#include <memory>

#include "core/infer/inferer.h"

namespace alg {

/* Implemented in src/backend/<chip>/<chip>_backend.cpp. CMake links exactly one. */
std::unique_ptr<IInferer> MakeInferer();
const char*               BackendName();

}  // namespace alg

#endif  // ALG_BACKEND_BACKEND_FACTORY_H
