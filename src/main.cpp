#ifdef __EMSCRIPTEN__

#include "renderer.hpp"

int main() { return portfolio::run(); }

#else

int main() {}

#endif
