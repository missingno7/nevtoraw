// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw.h"

namespace nev {

// Nikon TIFF raw containers (.NEF and .NRW).
bool read_nef(const std::string &path, RawPhoto &photo, std::string &err);

} // namespace nev
