#pragma once
#include "KeyBifArchive.hpp"
#include "LooseArchiveCatalog.hpp"
#include "ResourceQuery.hpp"
#include <neoshared/ResourceDocument.hpp>
namespace neobif {
neoshared::ResourceDocument readResourceDocument(
    const KeyBifArchive& key, const LooseArchiveCatalog& loose,
    const ResourceSelection& selection);
}
