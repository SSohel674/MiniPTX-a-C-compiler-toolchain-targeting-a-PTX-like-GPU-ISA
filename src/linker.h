#pragma once
#include <vector>
#include "object.h"

namespace mp {
// Merges relocatable objects: concatenates .text, resolves symbols across modules,
// applies relocations, rebases debug info. Produces an executable image.
Object linkObjects(const std::vector<Object>& objs);
}
