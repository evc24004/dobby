#pragma once

#if defined(__ANDROID__)
namespace dobby {

void registerDeveloperUi();
void showDeveloperStatus(void* = nullptr);
void showLatestViolation(void* = nullptr);
void requestLatestViolationPopup();
void showPendingViolationPopup();

} // namespace dobby
#endif
