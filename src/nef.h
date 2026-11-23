// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "raw.h"

namespace nev {

bool read_nef(const std::string &path, RawPhoto &photo, std::string &err);

} // namespace nev
