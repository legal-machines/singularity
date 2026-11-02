#include "Platform/MailDraft/MailDraft.hpp"

#import <AppKit/AppKit.h>

namespace singularity::platform {
namespace {

NSString *text_of(const std::string &value) {
  NSString *text = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
  return text != nil ? text : @"";
}

// Kept while Mail takes the message: the service hands it over after this call.
NSSharingService *composing = nil;

} // namespace

bool open_mail_draft(const std::string &to, const std::string &subject, const std::string &body,
                     const std::vector<std::filesystem::path> &files, const std::filesystem::path &eml) {
  @autoreleasepool {
    NSWorkspace *workspace = NSWorkspace.sharedWorkspace;
    NSURL *mailApp = [workspace URLForApplicationToOpenURL:[NSURL URLWithString:@"mailto:"]];
    NSString *mailAppId = mailApp != nil ? [NSBundle bundleWithURL:mailApp].bundleIdentifier : nil;
    if ([mailAppId isEqualToString:@"com.apple.mail"]) {
      NSSharingService *service = [NSSharingService sharingServiceNamed:NSSharingServiceNameComposeEmail];
      NSMutableArray *items = [NSMutableArray array];
      if (body.empty() == false) [items addObject:text_of(body)];
      for (const std::filesystem::path &file : files) {
        [items addObject:[NSURL fileURLWithPath:text_of(file.u8string())]];
      }
      if (service != nil && [service canPerformWithItems:items]) {
        service.recipients = @[ text_of(to) ];
        service.subject = text_of(subject);
        composing = service;
        [service performWithItems:items];
        return true;
      }
    }
    if (eml.empty()) return false;
    NSURL *draft = [NSURL fileURLWithPath:text_of(eml.u8string())];
    if (mailApp != nil) {
      [workspace openURLs:@[ draft ]
          withApplicationAtURL:mailApp
                 configuration:[NSWorkspaceOpenConfiguration configuration]
             completionHandler:nil];
      return true;
    }
    return [workspace openURL:draft];
  }
}

} // namespace singularity::platform
