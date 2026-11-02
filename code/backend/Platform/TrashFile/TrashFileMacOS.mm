#include "Platform/TrashFile/TrashFile.hpp"

#import <Foundation/Foundation.h>

#include <mutex>

namespace singularity::platform {
namespace {

std::mutex override_mutex;
std::optional<TrashResult> result_override;

} // namespace

const char *trash_name() { return "Trash"; }

TrashResult move_to_trash(const std::string& path) {
	{
		const std::lock_guard<std::mutex> lock(override_mutex);
		if (result_override) return *result_override;
	}

	@autoreleasepool {
		NSString *nsPath = [NSString stringWithUTF8String:path.c_str()];
		if (nsPath == nil) return TrashResult::Failed;
		NSURL *url = [NSURL fileURLWithPath:nsPath];
		NSError *error = nil;
		const BOOL ok = [[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:nil error:&error];
		return ok == YES ? TrashResult::Succeeded : TrashResult::Failed;
	}
}

void set_trash_result_override_for_testing(std::optional<TrashResult> value) {
	const std::lock_guard<std::mutex> lock(override_mutex);
	result_override = value;
}

} // namespace singularity::platform
