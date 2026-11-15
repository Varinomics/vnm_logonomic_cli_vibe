#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vibe {

struct Resume_record
{
    std::string session_id;
    std::filesystem::path home;
    std::filesystem::path transcript;
};

std::optional<Resume_record> decode_resume_record(const logonomic::cli_addon::Json_value& record);
std::optional<std::vector<std::string>> resume_arguments(const std::vector<std::string>& invocation);
logonomic::cli_addon::Resume_availability inspect_native_session(const Resume_record& record);
void handle_resume(logonomic::cli_addon::Invocation_context& context);

} // namespace vibe
