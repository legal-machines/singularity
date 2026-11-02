#pragma once

#include "Matter/MatterRepository/MatterRepository.hpp"

#include <filesystem>
#include <string>

namespace singularity::work_product {

struct PreparedExport {
  std::string content;
  std::string default_name;
  std::string filter_name;
  std::string extension;
};

PreparedExport prepare_export(const std::string &matter_id, const std::string &source_id,
                              matters::ExportSourceType source_type,
                              const std::string &format);
void write_export(const std::filesystem::path &path, const PreparedExport &prepared);

} // namespace singularity::work_product
