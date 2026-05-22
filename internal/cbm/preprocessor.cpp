// Unity build: include simplecpp implementation directly since CGo only
// compiles .cpp files from the immediate package directory, not subdirs.
#include "vendored/simplecpp/simplecpp.cpp"

#include "preprocessor.h"
#include <cstdlib>

extern "C" {

char* cbm_preprocess(
    const char* source, int source_len,
    const char* filename,
    const char** extra_defines,
    const char** include_paths,
    int cpp_mode
) {
    // Preprocessing disabled: simplecpp macro expansion causes unbounded
    // memory usage and hangs on heavily-templated C++ codebases (macOS).
    // Tree-Sitter parses source directly without macro expansion.
    (void)source; (void)source_len; (void)filename;
    (void)extra_defines; (void)include_paths; (void)cpp_mode;
    return NULL;
}

void cbm_preprocess_free(char* expanded) {
    free(expanded);
}

} // extern "C"
