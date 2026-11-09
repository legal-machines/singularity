#include "Chat/ConversationClearing/ConversationClearing.hpp"
#include "Agent/AgentActions/AgentActions.hpp"
#include "Agent/AgentProduction/AgentProduction.hpp"
#include "Agent/AgentScheduler/AgentScheduler.hpp"
#include "Agent/AgentStore/AgentStore.hpp"
#include "Agent/AiAppConnections/AiAppConnections.hpp"
#include "Matter/SharedMatter/SharedMatterService.hpp"
#include "Matter/SharedMatter/SharedMatter.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterCoordination/MatterCoordination.hpp"
#include "Matter/FirmComputers/FirmComputersProduction.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Bridge/AgentBridge/AgentBridge.hpp"
#include "Bridge/OrganisationBridge/OrganisationBridge.hpp"
#include "Organisation/FirmRole/FirmRole.hpp"
#include "Organisation/OrganisationProduction/OrganisationProduction.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/BackgroundActivity/BackgroundActivity.hpp"
#include "Platform/Events/Events.hpp"
#include "Chat/ChatExecution/ChatExecution.hpp"
#include "Chat/ChatActions/ChatActions.hpp"
#include "Context/MemoryKeeper/MemoryKeeper.hpp"
#include "Context/ProjectContextActions/ProjectContextActions.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Persistence/ProjectStore/ProjectStore.hpp"
#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"
#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"
#include "Bridge/NativeBridge/NativeBridge.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Tool/DeviceTrust/DeviceTrust.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Node/WorkLog/WorkLog.hpp"
#include "Node/WorkLogShare/WorkLogShare.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Tool/Types/ToolTypes.hpp"
#include "Bridge/AccountBridge/AccountBridge.hpp"
#include "Bridge/ChatBridge/ChatBridge.hpp"
#include "Bridge/ChatExecutionBridge/ChatExecutionBridge.hpp"
#include "Bridge/ChatPreviewBridge/ChatPreviewBridge.hpp"
#include "Bridge/ConsiliumBridge/ConsiliumBridge.hpp"
#include "Bridge/ContextBridge/ContextBridge.hpp"
#include "Bridge/DocumentBridge/DocumentBridge.hpp"
#include "Bridge/DocumentReviewBridge/DocumentReviewBridge.hpp"
#include "DocumentReview/DocumentReviewRun/DocumentReviewRun.hpp"
#include "Bridge/LocalModelBridge/LocalModelBridge.hpp"
#include "LocalModel/ModelPackageManager/ModelPackageManager.hpp"
#include "Bridge/MatterBridge/MatterBridge.hpp"
#include "Bridge/NodeBridge/NodeBridge.hpp"
#include "Bridge/PapinianGuardBridge/PapinianGuardBridge.hpp"
#include "Bridge/PlatformBridge/PlatformBridge.hpp"
#include "Bridge/PrivacyBridge/PrivacyBridge.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Bridge/UpdateBridge/UpdateBridge.hpp"
#include "Platform/Trace/Trace.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Privacy/PrivacyActions/PrivacyActions.hpp"
#include "Platform/StaleRuntimeReaper/StaleRuntimeReaper.hpp"
#include "Bridge/ProjectContextBridge/ProjectContextBridge.hpp"
#include "Bridge/ProviderBridge/ProviderBridge.hpp"
#include "Bridge/RoutingBridge/RoutingBridge.hpp"
#include "Bridge/UsageBridge/UsageBridge.hpp"
#include "Bridge/GeneratedFileBridge/GeneratedFileBridge.hpp"
#include "Bridge/WorkProductBridge/WorkProductBridge.hpp"
#include "Bridge/PlaybookBridge/PlaybookBridge.hpp"
#include "Bridge/ReviewTableBridge/ReviewTableBridge.hpp"
#include "Bridge/CorrespondenceBridge/CorrespondenceBridge.hpp"
#include "Bridge/TimeBridge/TimeBridge.hpp"
#include "Bridge/OfficeBridge/OfficeBridge.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AppWindow/AppWindow.hpp"
#include "ExternalResearch/SiteIcon/SiteIcon.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/Types/WindowLayout.hpp"
#include "Bridge/NativeBridge/BackgroundExecutor.hpp"
#include "Bridge/NativeBridge/BridgeCallTrace.hpp"
#include "Bridge/NativeBridge/NativeError.hpp"
#include "Platform/NativeDialogs/NativeDialogs.hpp"
#include "Platform/OpenExternal/OpenExternal.hpp"
#include "Platform/SeatPasswordStore/SeatPasswordStore.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Provider/AccountGate/AccountGate.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/MatterRequestExecution/MatterRequestExecution.hpp"
#include "Usage/LocalWork/LocalWork.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace singularity::bridge {
namespace {

using Json = nlohmann::json;

enum class ExecutionPolicy { UiThread, Worker };

// The one canonical Native API version, matching docs/contracts/native/
// Manifest.json's "apiVersion". Both the embedded JS client's apiVersion
// property (bridge_script(), below) and runtime_info()'s wire response read
// this same constant, so they cannot independently drift from each other -
// deploy/scripts/generate_native_contract.py's verify_implementation()
// checks this constant's value against the contract's own apiVersion.
constexpr int kNativeApiVersion = 106;

// The organisation this computer is signed in to, for the firm's
// seal; empty when it is signed in to none.
std::string signed_in_organisation() {
  const platform::AppSettings settings = platform::load_app_settings();
  return settings.account ? settings.account->organisation_id : std::string();
}

// Seals a Matter with this computer's key for its organisation.
void seal_with_this_computer(const std::filesystem::path &folder, const std::string &matter_id) {
  const platform::ActorIdentity identity = platform::resolve_actor_identity();
  matter::seal_matter(folder, matter_id, signed_in_organisation(), identity.actor_id, identity.device_id,
      workspaces::current_timestamp());
}

struct Handler final {
  ExecutionPolicy policy;
  std::function<Json(const Json &)> callback;
  // For a UiThread method whose dialog is only its first step: the rest of the
  // work, given what the dialog returned, runs on a worker so the window never
  // waits on a save or a Project being set up behind it.
  std::function<Json(const Json &)> continuation;
};

// Never fewer than two: a run holds its worker for as long as it waits on the
// lawyer - a question to answer, a web search to approve - and the window reads
// the question it must show through a worker too. With one, that read would
// queue behind the very run waiting for its answer.
std::size_t default_worker_count() {
  const auto available = std::thread::hardware_concurrency();
  if (available <= 2) {
    return 2;
  }
  return std::min<std::size_t>(4, available - 1);
}

// Requests that hold their thread for a whole run - every model call, and any
// wait inside it for the lawyer's answer or approval - run on a pool of their
// own. A run waiting on the lawyer must never leave the window without a
// thread to read what it has to show, which is the very question it waits on;
// with one pool, two such runs on a small computer would take every thread.
// They are still Worker methods in the contract: off the window's thread.
constexpr std::size_t kRunThreads = 4;

// The lawyer's controls over work already running - Stop, an approval, the
// answer to a question, and the one-second read of the card they act on - run
// on a pool of their own: never on the window's thread, where a save behind
// the Project's lock would freeze the whole window, and never queued behind
// the reads of the ordinary pool or the runs they control.
constexpr std::size_t kControlThreads = 2;

// A read the new frontend uses, with who is looking and what they may do
// beside it: the same viewer organisation.get holds, so a page relates every
// author and member it shows to the person reading it.
std::function<Json(const Json &)> with_viewer(std::function<Json(const Json &)> read) {
  return [read = std::move(read)](const Json &params) {
    Json result = read(params);
    if (result.is_object()) result["viewer"] = organisation_bridge::viewer();
    return result;
  };
}

bool is_operation_control(const std::string &method) {
  return method == "chat.stopOperation" || method == "chat.stopColleagues" || method == "chat.operationState" || method == "chat.respondToolApproval"
      || method == "chat.answerQuestion" || method == "reviewTable.stop" || method == "reviewTable.stopChanges";
}

bool holds_a_run(const std::string &method) {
  return method == "chat.send" || method == "chat.resumeOperation" || method == "matter.runTask"
      || method == "matter.retryTask" || method == "matter.reviseTask" || method == "matter.runConsilium"
      || method == "matter.synthesizeConsilium" || method == "matter.runWorkflow" || method == "reviewTable.run" || method == "reviewTable.prepareChanges"
      || method == "protectedApi.previewDocument";
}

// A whole-product test on a Debug build may stand in for Legal Machines'
// account at onboarding: NATIVE_APP_EMULATE_ACCOUNT=register for a new firm,
// =join for a colleague the firm invited, with NATIVE_APP_EMULATE_FIRM
// naming the firm joined. The page's account stand-in
// (onboarding/entry/account.js) answers then, and nothing is asked of Legal
// Machines. Every other build compiles it out: its account's checks are
// never weakened.
std::string account_emulation_script() {
#if defined(SINGULARITY_ACCOUNT_EMULATION)
  const char *mode = std::getenv("NATIVE_APP_EMULATE_ACCOUNT");
  const std::string chosen = mode != nullptr ? mode : "";
  if (chosen != "register" && chosen != "join") return std::string();
  const char *firm = std::getenv("NATIVE_APP_EMULATE_FIRM");
  const auto literal = [](const std::string &value) {
    return Json(value).dump(-1, ' ', false, Json::error_handler_t::replace);
  };
  return "    accountEmulation: " + literal(chosen) + ",\n    emulatedFirm: "
      + literal(firm != nullptr ? std::string(firm) : std::string()) + ",\n";
#else
  return std::string();
#endif
}

// window_frame names who draws the window's caption, for the page's
// data-native-window-frame: "page" or "integrated" (AppWindow::frame).
std::string bridge_script(const std::string &window_frame) {
  return std::string(R"js(
(() => {
  const transport = window.__nativeInvoke.bind(window);
  try { delete window.__nativeInvoke; } catch (_) {}

  const events = new EventTarget();
  // Diagnostic trace (SINGULARITY_TRACE_FILE): what each native call cost as the
  // interface experienced it - the wait for the app's main thread included -
  // and the moments the interface itself marks. Absent when the trace is off.
  const traceOn = )js" + std::string(platform::trace::enabled() ? "true" : "false") + R"js(;
  const traceBuffer = [];
  const traceNote = (entry) => {
    if (traceOn !== true) return;
    traceBuffer.push(entry);
    if (traceBuffer.length === 1) {
      setTimeout(() => {
        const entries = traceBuffer.splice(0, traceBuffer.length);
        transport('__trace', { entries }).catch(() => {});
      }, 400);
    }
  };
  if (traceOn === true) {
    window.__lmTraceMark = (name, fields = {}) =>
      traceNote({ kind: 'mark', name: String(name), at: performance.now(), fields });
  }
  const invoke = async (method, params = {}) => {
    const startedAt = traceOn === true ? performance.now() : 0;
    try {
      const result = await transport(method, params);
      if (traceOn === true) traceNote({ kind: 'call', name: method, at: startedAt, ms: performance.now() - startedAt, ok: true });
      return result;
    } catch (reason) {
      if (traceOn === true) traceNote({ kind: 'call', name: method, at: startedAt, ms: performance.now() - startedAt, ok: false });
      if (reason && typeof reason === 'object' && 'message' in reason) {
        // The error's message is the sentence written for the lawyer; what
        // was recorded about the failure, word for word, is its detail, for a
        // closed "Details" only (NativeError.hpp).
        const recorded = String(reason.message);
        const userMessage = typeof reason.userMessage === 'string' && reason.userMessage !== '' ? reason.userMessage : '';
        const error = new Error(userMessage !== '' ? userMessage : recorded);
        error.detail = recorded;
        error.userMessage = userMessage;
        error.code = typeof reason.code === 'string' && reason.code !== '' ? reason.code : 'native.handler_failed';
        error.retryable = reason.retryable === true;
        error.degradedCapability = typeof reason.degradedCapability === 'string'
          ? reason.degradedCapability
          : null;
        throw error;
      }
      throw reason;
    }
  };

  const api = Object.freeze({
    apiVersion: )js") +
         std::to_string(kNativeApiVersion) + R"js(,
    platform: ')js" +
         platform::contract_platform_name() + R"js(',
    // Who draws the window's caption: 'page' (the page draws minimize,
    // maximize and close, and its title bar drags the window) or
    // 'integrated' (the window keeps its native controls).
    windowFrame: ')js" +
         window_frame + R"js(',
)js" + account_emulation_script() + R"js(
    chooseFolder: () => invoke('dialog.chooseFolder'),
    // Phase 09's explicit-picker path for "Add external file" - resolves
    // real absolute paths only, [] if the picker is cancelled.
    chooseFiles: () => invoke('dialog.chooseFiles'),
    getPermissions: () => invoke('permissions.getState'),
    chooseFilesAndProjectsAccess: () => invoke('permissions.chooseFilesAndProjects'),
    declineFilesAndProjectsAccess: () => invoke('permissions.declineFilesAndProjects'),
    allowBroaderAccess: () => invoke('permissions.allowBroaderAccess'),
    revokeBroaderAccess: () => invoke('permissions.revokeBroaderAccess'),
    getRuntimeInfo: () => invoke('runtime.getInfo'),
    getBuildHash: () => invoke('runtime.getBuildHash'),
    // Singularity's new versions from Legal Machines' site (Node/AppUpdate).
    getUpdateStatus: () => invoke('update.status', {}),
    checkForUpdate: () => invoke('update.check', {}),
    downloadUpdate: () => invoke('update.download', {}),
    showUpdate: () => invoke('update.show', {}),
    installUpdate: () => invoke('update.install', {}),
    getOnboardingStatus: () => invoke('onboarding.getStatus'),
    completeOnboarding: () => invoke('onboarding.complete'),
    prepareMainWindow: () => invoke('window.prepareMain'),
    // The window's frame for a page that draws its own header:
    // { captionInPage, maximized }. With the caption in the page (Windows),
    // the page draws minimize, maximize and close.
    getWindowFrame: () => invoke('window.getFrame'),
    // Runs a command of the window's caption from the page's own button:
    // 'minimize', 'maximize', 'restore' or 'close'.
    runWindowCaptionCommand: (command) => invoke('window.captionCommand', { command }),
    // Where the page has put its own controls inside the strip at the top of
    // the window that otherwise drags it: [{ x, y, width, height }] in CSS
    // pixels. A press inside one reaches the page; [] gives the strip back.
    setTitlebarControls: (controls) => invoke('window.setTitlebarControls', { controls }),
    // contactEmailMasked/contactEmailVerified/displayName/organisationName
    // are a patch, like saveLocalSetup's patch object - omit any to keep its
    // previously cached value (e.g. a plain sign-in only touches
    // organisationId/seatId).
    saveAccountResume: (organisationId, seatId, contactEmailMasked, contactEmailVerified, displayName, organisationName) =>
      invoke('onboarding.saveAccountResume', {
        organisationId,
        seatId,
        contactEmailMasked,
        contactEmailVerified,
        displayName,
        organisationName,
      }),
    saveLocalSetup: (patch) => invoke('onboarding.saveLocalSetup', patch),
    // Legal defaults are non-secret local settings; provider credentials and
    // Papinian restoration data never cross this renderer contract.
    getLegalInstructionSettings: () => invoke('settings.getLegalInstructions'),
    // The name and firm of the person working on this computer (Settings).
    savePerson: (displayName, organisationName) => invoke('settings.savePerson', { displayName, organisationName }),
    saveLegalInstructionSettings: (userDefaultLegalInstructions) =>
      invoke('settings.saveLegalInstructions', { userDefaultLegalInstructions }),
    getDismissedNoticeIds: () => invoke('notices.getDismissedIds'),
    dismissNotice: (noticeId) => invoke('notices.dismiss', { noticeId }),
    listWorkspaces: () => invoke('workspace.list'),
    chooseWorkspace: () => invoke('workspace.choose'),
    removeWorkspace: (id) => invoke('workspace.remove', { id }),
    setActiveWorkspace: (id) => invoke('workspace.setActive', { id }),
    relocateWorkspace: (id) => invoke('workspace.relocate', { id }),
    listProviders: () => invoke('providers.list'),
    configureProvider: (providerId, secret, baseUrl, displayName) =>
      invoke('providers.configure', { providerId, secret, baseUrl, displayName }),
    disconnectProvider: (providerId) => invoke('providers.disconnect', { providerId }),
    // The backend verifies the key against the provider's own API and
    // discovers its model list itself - see Provider/ProviderRegistry/ProviderRegistry.*
    // - so the renderer never needs to send more than the key (and, for
    // an OpenAI-compatible endpoint, its base URL).
    getProtectedApiPolicy: () => invoke('protectedApi.getPolicy'),
    saveProtectedApiPolicy: (policy) => invoke('protectedApi.savePolicy', policy),
    previewProtectedApi: (text) => invoke('protectedApi.preview', { text }),
    previewProtectedPassage: (projectId, text) => invoke('protectedApi.previewPassage', { projectId, text }),
    // One document as a model receives it: each passage as written and as
    // masked, and each symbol with the words it stands for.
    previewProtectedDocument: (projectId, documentId) => invoke('protectedApi.previewDocument', { projectId, documentId }),
    getPapinianModelStatus: () => invoke('papinian.getModelStatus'),
    // Signing in to the Legal Machines account, which lets this computer
    // download the protection model. The password goes to the native side for
    // this one call and is never kept; nothing here returns a token.
    signInToLegalMachines: (organisationId, seatId, password) =>
      invoke('account.signIn', { organisationId, seatId, password }),
    verifyLegalMachinesCode: (challengeId, code, organisationId, seatId, password, email) =>
      invoke('account.verifyTwoFactor', { challengeId, code, organisationId, seatId, password, email }),
    getAccountState: () => invoke('account.getState'),
    signOutOfLegalMachines: () => invoke('account.signOut'),
    // Onboarding begins with the organisation's email: a known one signs in with
    // a seat and its password; a new one registers and confirms its email.
    findLegalMachinesOrganisation: (email) => invoke('account.lookup', { email }),
    signInToLegalMachinesWithEmail: (email, seat, password) =>
      invoke('account.signInWithEmail', { email, seat, password }),
    // A seat's password kept by this computer's user's own store, at the
    // member's choice: the page learns only whether one is kept, and a sign-in
    // asks for it to be used natively; no call ever answers with it.
    getSeatPasswordStore: () => invoke('account.seatPasswordStore', {}),
    hasSavedSeatPassword: (email, seat) => invoke('account.savedSeatPassword', { email, seat }),
    saveSeatPassword: (email, seat, password) => invoke('account.saveSeatPassword', { email, seat, password }),
    forgetSeatPassword: (email, seat) => invoke('account.forgetSeatPassword', { email, seat }),
    signInToLegalMachinesWithSavedPassword: (email, seat) =>
      invoke('account.signInWithEmail', { email, seat, savedPassword: true }),
    verifyLegalMachinesCodeWithSavedPassword: (challengeId, code, organisationId, seatId, email) =>
      invoke('account.verifyTwoFactor', { challengeId, code, organisationId, seatId, email, savedPassword: true }),
    registerLegalMachinesOrganisation: (registration) => invoke('account.register', registration),
    // A new organisation one thing a page: created from its details and
    // agreements, then, once its email is confirmed, its administrator's
    // password, an authenticator app and recovery codes.
    createLegalMachinesOrganisation: (registration) => invoke('account.create', registration),
    setLegalMachinesPassword: (organisationId, password) =>
      invoke('account.setPassword', { organisationId, password }),
    startLegalMachinesTwoFactor: (organisationId) => invoke('account.startTwoFactor', { organisationId }),
    // An organisation active at the end of its setup signs in here at once
    // with the password just set, which goes native for this one call and is
    // never kept.
    confirmLegalMachinesTwoFactor: (organisationId, code, seatId, password, email) =>
      invoke('account.confirmTwoFactor', { organisationId, code, seatId, password, email }),
    saveLegalMachinesRecoveryCodes: (organisationId) => invoke('account.saveRecoveryCodes', { organisationId }),
    printLegalMachinesRecoveryCodes: (organisationId) => invoke('account.printRecoveryCodes', { organisationId }),
    confirmLegalMachinesEmail: (organisationId, code) =>
      invoke('account.confirmEmail', { organisationId, code }),
    resendLegalMachinesEmailCode: (organisationId) =>
      invoke('account.resendEmailCode', { organisationId }),
    // Whether Legal Machines approved the organisation the window waits for.
    getLegalMachinesApprovalState: (organisationId) =>
      invoke('account.approvalState', { organisationId }),
    // A firm's seats: its administrator makes one for a colleague and hands
    // over the firm's email, the seat and its password.
    listFirmSeats: () => invoke('account.listSeats', {}),
    createFirmSeat: (seatId, password) => invoke('account.createSeat', { seatId, password }),
    setFirmSeatPassword: (seatId, password) => invoke('account.setSeatPassword', { seatId, password }),
    removeFirmSeat: (seatId) => invoke('account.removeSeat', { seatId }),
    // A seat's sign-in security: its own two-step and password, a
    // colleague's two-step turned off by the administrator, and the
    // administrator's recovery by the firm's email.
    getOwnSeat: () => invoke('account.ownSeat', {}),
    startOwnTwoFactor: () => invoke('account.startOwnTwoFactor', {}),
    confirmOwnTwoFactor: (code) => invoke('account.confirmOwnTwoFactor', { code }),
    replaceOwnTwoFactor: (password, code) => invoke('account.replaceOwnTwoFactor', { password, code }),
    confirmReplacedTwoFactor: (code) => invoke('account.confirmReplacedTwoFactor', { code }),
    renewOwnRecoveryCodes: (password, code) => invoke('account.renewOwnRecoveryCodes', { password, code }),
    changeOwnPassword: (currentPassword, newPassword, code) => invoke('account.changeOwnPassword', { currentPassword, newPassword, code }),
    resetSeatTwoFactor: (seatId) => invoke('account.resetSeatTwoFactor', { seatId }),
    requestAccountRecovery: (email) => invoke('account.requestRecovery', { email }),
    resetAccountPassword: (email, code, twoStepCode, newPassword) => invoke('account.resetPassword', { email, code, twoStepCode, newPassword }),
    reportProblem: (report) => invoke('account.reportProblem', report),
    getWorkLogPreview: (matterId, chatId) => invoke('account.workLogPreview', { matterId, chatId }),
    shareWorkLog: (log) => invoke('account.shareWorkLog', log),
    // A colleague archived, restored with a new password, or removed.
    setSeatState: (seatId, state, password) => invoke('organisation.setSeatState', { seatId, state, password }),
    authorizeLocalModels: (password) => invoke('localModels.authorize', { password }),
		syncLocalModels: (modelId, existingArtifactPath) =>
			invoke('localModels.sync', { modelId, existingArtifactPath }),
    listLocalModels: () => invoke('localModels.list'),
    getLocalRuntimeState: () => invoke('localModels.getRuntimeState'),
    loadLocalModel: (modelId) => invoke('localModels.load', { modelId }),
    unloadLocalModel: (modelId) => invoke('localModels.unload', { modelId }),
    removeLocalModel: (modelId) => invoke('localModels.remove', { modelId }),
    useExistingLocalModelFile: () => invoke('localModels.useExistingFile'),
    forgetExistingLocalModelFile: (modelId) => invoke('localModels.forgetExistingFile', { modelId }),
    getNodeDeviceSummary: () => invoke('node.getSummary'),
    checkLegalMachinesConnection: () => invoke('node.checkLegalMachinesConnection'),
    releaseReclaimableMemory: () => invoke('node.releaseReclaimableMemory'),
)js"
// Adjacent literals: MSVC rejects a single string literal over 16,380 bytes.
R"js(
    // Phase 06 - role/capability metadata for AI Setup pickers/drop targets, and the one
    // effective-setup resolver every AI Setup surface reads from (Node default -> Project
    // override -> Chat override). chatId requires projectId - see
    // routing_bridge::get_effective_ai_setup.
    getAiSetupCapabilities: () => invoke('routing.getAiSetupCapabilities'),
    getEffectiveAiSetup: (projectId, chatId) => invoke('routing.getEffectiveAiSetup', { projectId, chatId }),
    listMatters: () => invoke('matter.list'),
    listMatterSummaries: () => invoke('matter.listSummaries'),
    // Master Spec Sec.62/63 - safe operational metadata already persisted
    // after every real task attempt (Routing/RoutingLog/RoutingLog.hpp), never an API
    // key/Authorization header/raw restoration mapping.
    getRoutingLog: () => invoke('routingLog.list'),
    getUsageSummary: (periodStart, periodEnd, projectId) =>
      invoke('usage.getSummary', { periodStart, periodEnd, projectId }),
    generateProjectUsageStatement: (projectId, periodStart, periodEnd, structuredFormat = 'json') =>
      invoke('usage.generateProjectStatement', { projectId, periodStart, periodEnd, structuredFormat }),
    // A Matter's month of AI work as a statement for its client: a PDF and its
    // signed file, in the folder the lawyer chooses.
    saveWorkStatement: (projectId, periodStart, periodEnd, periodName) =>
      invoke('usage.saveWorkStatement', { projectId, periodStart, periodEnd, periodName }),
    confirmWorkCosts: (projectId, periodStart, periodEnd) =>
      invoke('usage.confirmCosts', { projectId, periodStart, periodEnd }),
    createMatter: (name, description) => invoke('matter.create', { name, description }),
    // The direct "New Project" flow (Beta UX correction): one folder picker,
    // one real Project created and assigned to that exact folder - never a
    // separate broader-access ceremony first.
    createProjectFromFolder: () => invoke('matter.createFromFolder'),
    createSampleProject: () => invoke('project.createSample'),
    createNamedProject: (params) => invoke('project.create', params),
    // The folder a new Matter lives in, chosen in the system's dialog: its
    // path stays here, the window holds a token for it.
    chooseNewMatterFolder: () => invoke('matter.chooseNewFolder', {}),
    updateMatter: (id, patch) => invoke('matter.update', { id, ...patch }),
    deleteMatter: (id) => invoke('matter.delete', { id }),
    listMatterTrash: () => invoke('project.listTrash', {}),
    moveMatterToTrash: (id) => invoke('project.moveToTrash', { id }),
    restoreMatterFromTrash: (id) => invoke('project.restoreFromTrash', { id }),
    deleteMatterFromTrash: (id) => invoke('project.deleteFromTrash', { id }),
    addMatterDocument: (matterId) => invoke('matter.addDocument', { matterId }),
    removeMatterDocument: (matterId, documentId) =>
      invoke('matter.removeDocument', { matterId, documentId }),
    revealMatterDocument: (matterId, documentId) =>
      invoke('matter.revealDocument', { matterId, documentId }),
    // Phase 09's "Open document" - the OS default application, not an
    // in-app viewer. See Platform/OpenExternal/OpenExternal.hpp.
    openMatterDocument: (matterId, documentId) => invoke('matter.openDocument', { matterId, documentId }),
		// Phase 08 (Documents - filesystem, list and states): reconciles and
		// returns the Matter's real Project-folder documents - see
		// Document/DocumentRegistry/DocumentRegistry.hpp.
		listMatterDocuments: (matterId) => invoke('matter.listDocuments', { matterId }),
    // Phase 09 real filesystem actions for a Referenced document - see
    // Document/DocumentFileActions/DocumentFileActions.hpp. expectedVersion
    // is the document's current version as last observed by the caller: a
    // stale selection (the document changed since) refuses instead of
    // silently acting on the wrong bytes.
    renameMatterDocument: (matterId, documentId, expectedVersion, newName) =>
      invoke('matter.renameDocument', { matterId, documentId, expectedVersion, newName }),
    moveMatterDocument: (matterId, documentId, expectedVersion, destinationFolderRelativePath) =>
      invoke('matter.moveDocument', { matterId, documentId, expectedVersion, destinationFolderRelativePath }),
    // Deletes only via the real OS Trash - rejects instead of resolving if
    // Trash is unsupported here or the OS call fails, so the UI never has a
    // "permanent delete" outcome to (mis)represent.
    deleteMatterDocument: (matterId, documentId, expectedVersion) =>
      invoke('matter.deleteDocument', { matterId, documentId, expectedVersion }),
    createMatterFolder: (matterId, parentFolderRelativePath, folderName) =>
      invoke('matter.createFolder', { matterId, parentFolderRelativePath, folderName }),
    // `move` false (default) copies the external file into the Project;
    // `move` true removes the original only after a successful copy.
    // `keepBoth` gives a taken name the next free one instead of refusing.
    addExternalMatterFile: (matterId, destinationFolderRelativePath, sourceAbsolutePath, move = false, keepBoth = false) =>
      invoke('matter.addExternalFile', keepBoth ? { matterId, destinationFolderRelativePath, sourceAbsolutePath, move, keepBoth }
        : { matterId, destinationFolderRelativePath, sourceAbsolutePath, move }),
    addExternalMatterFolder: (matterId, destinationFolderRelativePath, sourceAbsolutePath) =>
      invoke('document.addExternalFolder', { matterId, destinationFolderRelativePath, sourceAbsolutePath }),
    // "Try again" on a document stuck at Needs attention - a real
    // re-invocation of the extraction pipeline, never a fake success.
    reprocessMatterDocument: (matterId, documentId) => invoke('matter.reprocessDocument', { matterId, documentId }),
    // Phase 09's local source-resolution contract
    // (Document/DocumentSourceResolver/DocumentSourceResolver.hpp) -
    // chunkId omitted/"" resolves the Document's own first real chunk
    // ("Preview source"); a real chunkId (document_id + chunk_id, exactly
    // what a real citation carries) resolves that exact historical
    // reference, honestly reporting "changed" rather than silently binding
    // it to different current bytes.
    resolveDocumentSource: (matterId, documentId, chunkId = "", extractorVersion = null) =>
      invoke('matter.resolveDocumentSource', { matterId, documentId, chunkId, extractorVersion }),
    // The document as its author laid it out, read fresh from the real
    // current file - what a lawyer looks at inside Singularity after a change,
    // instead of opening Word to find out what happened.
    readDocumentView: (matterId, documentId) =>
      invoke('matter.readDocumentView', { matterId, documentId }),
    // The versions Singularity kept of a document before it changed it, and
    // putting one back - only while the document still holds
    // expectedFingerprint, the version the lawyer was looking at.
    listDocumentVersions: (projectId, documentId) => invoke('document.listVersions', { projectId, documentId }),
    restoreDocumentVersion: (projectId, documentId, version, expectedFingerprint) =>
      invoke('document.restoreVersion', { projectId, documentId, version, expectedFingerprint }),
    listProjectChats: (projectId) => invoke('chat.list', { projectId }),
    createProjectChat: (projectId) => invoke('chat.create', { projectId }),
    renameProjectChat: (projectId, chatId, expectedRevision, title) =>
      invoke('chat.rename', { projectId, chatId, expectedRevision, title }),
    // Phase 27's restart-surviving draft (unsent composer text) - never
    // sent automatically, never visible from another Chat.
    saveChatDraft: (projectId, chatId, draftText) =>
      invoke('chat.saveDraft', { projectId, chatId, draftText }),
    // The one document this Chat works on - persisted, so a change a lawyer
    // asks for lands in the file they chose rather than in whichever file
    // they happened to name in the sentence.
    setChatWorkingDocument: (projectId, chatId, documentId) =>
      invoke('chat.setWorkingDocument', { projectId, chatId, documentId }),
    // Phase 27's real backend dependency check before Chat deletion - what
    // a Delete Chat confirmation honestly tells the user before it happens.
    getChatDeletionImpact: (projectId, chatId) =>
      invoke('chat.deletionImpact', { projectId, chatId }),
    // A real removal from Project.chats, never merely the separate
    // `archived` hide-from-list state.
    deleteProjectChat: (projectId, chatId, expectedRevision) =>
      invoke('chat.delete', { projectId, chatId, expectedRevision }),
    restoreChat: (projectId, chatId) => invoke('chat.restore', { projectId, chatId }),
    // A thread archived or brought back.
    setChatArchived: (projectId, chatId, archived) => invoke('chat.setArchived', { projectId, chatId, archived }),
    // "/compact" - carry a long conversation forward in a shorter form.
    compactChat: (projectId, chatId) => invoke('chat.compact', { projectId, chatId }),
    // Phase 27's Search - a real backend text match over Chat title/
    // prompts/response text, never a frontend-only filter.
    searchProjectChats: (projectId, query) =>
      invoke('chat.search', { projectId, query }),
    // selectedDocumentIds is this Chat's persisted starting source policy
    // (Phase 09's "Ask in Chat") - canonical Document IDs only.
    createMatterChat: (matterId, title, selectedDocumentIds = []) =>
      invoke('matter.createChat', { matterId, title, selectedDocumentIds }),
    // Persists this Chat's explicit AI Setup differences (Phase 06) - an
    // empty overrides array is "Reset to Project setup", never a delete of
    // the Chat itself. See chat_bridge::save_ai_setup().
    saveChatAiSetup: (matterId, chatId, overrides) =>
      invoke('matter.saveChatAiSetup', { matterId, chatId, overrides }),
    // Phase 21's real backend-owned source-policy resolution (Chat/
    // ChatSourceResolver/ChatSourceResolver.hpp) - sourcePolicyMode is
    // explicit backend truth, never inferred from selectedDocumentIds'
    // shape: "relevant" requires an empty selection (local retrieval across
    // every AI-ready Project document - never the whole Project);
    // "selected" requires a non-empty selection and is a strict
)js"
// Adjacent literals: MSVC rejects a single string literal over 16,380 bytes.
R"js(    // backend-enforced boundary; "selected" with an empty selection fails
    // closed and never silently falls back to "relevant". Composition/
    // source preparation only - never runs a model or creates a MatterTask.
    resolveChatSources: (projectId, query, sourcePolicyMode, selectedDocumentIds = []) =>
      invoke('chat.resolveSources', { projectId, query, sourcePolicyMode, selectedDocumentIds }),
    // Phase 22's backend-owned legal task truth (Routing/LegalTaskPresets/
    // LegalTaskPresets.hpp's selectable_legal_task_presets()) - the current
    // (id, version, name) for every task Chat's composer may offer through
    // progressive disclosure. Never includes General.
    getChatLegalTaskOptions: () => invoke('chat.legalTaskOptions'),
    // Phase 22's narrow pre-execution resolution boundary (Chat/
    // ChatOperationSetup/ChatOperationSetup.hpp) - resolves, but never
    // executes, the legal task/preset, primary result-producing role/model
    // and full effective role assignment set a real operation would use.
    // params: { projectId, chatId?, legalTaskPresetId?,
    // legalTaskPresetVersion?, oneTimePrimaryModelOverride?: { providerId,
    // modelId } }.
    resolveChatOperationSetup: (params) => invoke('chat.resolveOperationSetup', params),
    // Phase 23's calm composer trust state (Chat/ChatOutboundPreview/
    // ChatOutboundPreview.hpp) - whether this Chat's next request would
    // leave this Mac at all, plus the real live Papinian Guard mode and
    // readiness it would run under. Deliberately independent of the prompt:
    // typing never prepares a request. params: { projectId, chatId?,
    // legalTaskPresetId?, legalTaskPresetVersion?,
    // oneTimePrimaryModelOverride?: { providerId, modelId } }.
    resolveChatProtectionState: (params) => invoke('chat.resolveProtectionState', params),
    // Phase 23's on-demand "What leaves this device?" for one pending Chat
    // request. Resolves Phase 21 sources and Phase 22 operation setup, then
    // prepares and previews the request through the exact same canonical
    // backend path a real send uses - the renderer composes nothing. Makes
    // no provider/network call, creates no MatterTask and persists nothing.
    // params: { projectId, chatId?, prompt, sourcePolicyMode,
    // selectedDocumentIds, legalTaskPresetId?, legalTaskPresetVersion?,
    // oneTimePrimaryModelOverride?: { providerId, modelId } }.
    previewChatOutboundRequest: (params) => invoke('chat.previewOutboundRequest', params),
    // Phase 24's one real Chat Send. The backend prepares the composer's
    // CURRENT state again through the same prepare_chat_request() Review
    // uses, then runs it through the existing canonical execution path -
    // one Send is one backend operation, and a previously prepared preview
    // is never executed. The renderer chooses no provider, model, source or
    // protection setting; operationId is only an opaque handle so Stop can
    // name this operation while it runs. params: { projectId, chatId?,
    // operationId, prompt, sourcePolicyMode, selectedDocumentIds,
    // legalTaskPresetId?, legalTaskPresetVersion?,
    // oneTimePrimaryModelOverride?: { providerId, modelId } }.
    sendChatRequest: (params) => invoke('chat.send', params),
	resumeChatOperation: (projectId, operationId) =>
	  invoke('chat.resumeOperation', { projectId, operationId }),
    // A real, backend-owned cooperative cancellation request. It never
    // claims that provider compute or an in-flight request was torn down:
    // the returned state says whether cancellation has taken effect or the
    // operation is still stopping.
    stopChatOperation: (projectId, operationId) =>
      invoke('chat.stopOperation', { projectId, operationId }),
    // The real lifecycle of one operation, straight from backend state -
    // never a timer-driven or invented stage.
    getChatOperationState: (projectId, operationId) =>
      invoke('chat.operationState', { projectId, operationId }),
    // Unfinished work this Project still has, after any restart.
    listChatOperations: (projectId) => invoke('chat.listOperations', { projectId }),
    getProjectActionPermission: (projectId) =>
      invoke('chat.actionPermission', { projectId }),
    saveProjectActionPermission: (projectId, requestedMode) =>
      invoke('chat.saveActionPermission', { projectId, requestedMode }),
    respondChatToolApproval: (projectId, operationId, approvalId, decision) =>
      invoke('chat.respondToolApproval', { projectId, operationId, approvalId, decision }),
    // The lawyer's answer to the question a running operation is waiting on -
    // or null to leave its decisions to Singularity. The same run goes on with it;
    // there is nothing to resume.
    answerChatQuestion: (projectId, operationId, questionId, answers) =>
      invoke('chat.answerQuestion', { projectId, operationId, questionId, answers }),
    // The read-only Chat conversation projected over canonical MatterTasks
    // (Chat/ChatConversation/ChatConversation.hpp). Response text arrives
    // already split around backend-validated citations, so the renderer
    // never sees or parses internal source-marker syntax. There is no second
    // Chat transcript or result store behind this.
    listChatConversation: (projectId, chatId) =>
      invoke('chat.conversation', { projectId, chatId }),
    // Source Peek: the backend verifies the citation against the canonical
    // Project/Chat/MatterTask chain and resolves it through the existing
    // local document source resolver. Degrades honestly to changed/
    // unavailable rather than inventing a page, heading or excerpt.
    resolveChatSourceReference: (projectId, chatId, taskId, documentId, chunkId) =>
      invoke('chat.resolveSourceReference', { projectId, chatId, taskId, documentId, chunkId }),
    chooseMatterFolder: (matterId) => invoke('matter.chooseFolder', { matterId }),
    openMatterFolder: (matterId) => invoke('matter.openFolder', { matterId }),
    addMatterNote: (matterId, content) => invoke('matter.addNote', { matterId, content }),
    removeMatterNote: (matterId, noteId) => invoke('matter.removeNote', { matterId, noteId }),
    addIssue: (matterId, findingId) => invoke('matter.addIssue', { matterId, findingId }),
    updateIssueStatus: (matterId, issueId, status) =>
      invoke('matter.updateIssueStatus', { matterId, issueId, status }),
		listReviewTables: (projectId) => invoke('reviewTable.list', { projectId }),
		getReviewTable: (projectId, tableId) => invoke('reviewTable.get', { projectId, tableId }),
		createReviewTable: (projectId, title, documentIds, columns, localOnly) =>
			invoke('reviewTable.create', { projectId, title, documentIds, columns, localOnly }),
		updateReviewTable: (projectId, tableId, changes) => invoke('reviewTable.update', { projectId, tableId, ...changes }),
		removeReviewTable: (projectId, tableId) => invoke('reviewTable.remove', { projectId, tableId }),
		runReviewTable: (projectId, tableId, providerId, modelId) =>
			invoke('reviewTable.run', { projectId, tableId, providerId, modelId }),
		stopReviewTable: (projectId, tableId) => invoke('reviewTable.stop', { projectId, tableId }),
		exportReviewTable: (projectId, tableId, format) => invoke('reviewTable.export', { projectId, tableId, format }),
		prepareReviewTableChanges: (projectId, tableId, documentIds) =>
			invoke('reviewTable.prepareChanges', { projectId, tableId, documentIds }),
		stopReviewTableChanges: (projectId, tableId) => invoke('reviewTable.stopChanges', { projectId, tableId }),
)js"
R"js(		listPlaybooks: () => invoke('playbook.list', {}),
		getPlaybook: (playbookId, version) => invoke('playbook.get', { playbookId, version }),
		savePlaybook: (playbook) => invoke('playbook.save', { playbook }),
		removePlaybook: (playbookId) => invoke('playbook.remove', { playbookId }),
		importPlaybook: (text) => invoke('playbook.import', { text }),
		exportPlaybook: (playbookId, version) => invoke('playbook.export', { playbookId, version }),
		reviewTableFromPlaybook: (projectId, playbookId, documentIds, title, localOnly) =>
			invoke('playbook.reviewTable', { projectId, playbookId, documentIds, title, localOnly }),
		listWorkProductTypes: (projectId) => invoke('workProduct.listTypes', { projectId }),
		createWorkProductType: (projectId, name, description, instructions, expectedStructure) =>
			invoke('workProduct.createType', { projectId, name, description, instructions, expectedStructure }),
		listWorkProducts: (projectId) => invoke('workProduct.list', { projectId }),
		getWorkProduct: (projectId, workProductId) =>
			invoke('workProduct.get', { projectId, workProductId }),
		createWorkProduct: (projectId, typeId, title, content) =>
			invoke('workProduct.create', { projectId, typeId, title, content }),
		retainTaskAsWorkProduct: (projectId, taskId, typeId, title) =>
			invoke('workProduct.retainTaskResult', { projectId, taskId, typeId, title }),
		updateWorkProduct: (projectId, workProductId, expectedRevision, title, content) =>
			invoke('workProduct.update', { projectId, workProductId, expectedRevision, title, content }),
		updateWorkProductIssueStatus: (projectId, workProductId, expectedRevision, status) =>
			invoke('workProduct.updateIssueStatus', { projectId, workProductId, expectedRevision, status }),
		restoreWorkProductRevision: (projectId, workProductId, expectedRevision, revision) =>
			invoke('workProduct.restoreRevision', { projectId, workProductId, expectedRevision, revision }),
		removeWorkProduct: (projectId, workProductId) =>
			invoke('workProduct.remove', { projectId, workProductId }),
		// Agents: standing work on this computer for one Matter (Agent.json).
		listAgents: (projectId) => invoke('agent.list', projectId == undefined ? {} : { projectId }),
		getAgent: (agentId) => invoke('agent.get', { agentId }),
		// The roles a colleague can be begun in, from the one catalogue.
		getAgentRoles: () => invoke('agent.roles', {}),
		// The organisation: people and AI colleagues, their areas, work and decisions.
		getOrganisation: () => invoke('organisation.get', {}),
		setOwnOffice: (office) => invoke('organisation.setOwnOffice', Object.assign({}, office)),
		// The photo the member shows the firm: kept here and in their record
		// of every Matter, never sent to Legal Machines.
		setOwnPhoto: (photo) => invoke('organisation.setOwnPhoto', { photo: photo == null ? null : photo }),
		chooseOwnPhoto: () => invoke('organisation.chooseOwnPhoto', {}),
		getAgentRun: (agentId, runId) => invoke('agent.getRun', { agentId, runId }),
		createAgent: (settings) => invoke('agent.create', settings),
		// A colleague of the firm, in no Matter.
		hireColleague: (colleague) => invoke('agent.hire', Object.assign({}, colleague)),
		getColleagueMemory: (agentId) => invoke('agent.memory', { agentId }),
		listFirmColleagues: () => invoke('agent.firmColleagues', {}),
		forgetColleagueMemory: (agentId, noteId) => invoke('agent.forgetMemory', { agentId, noteId }),
		approveAgent: (agentId, settings) => invoke('agent.approve', Object.assign({}, settings, { agentId })),
		declineAgent: (agentId) => invoke('agent.decline', { agentId }),
		pauseAgent: (agentId) => invoke('agent.pause', { agentId }),
		setAgentModel: (agentId, providerId, modelId) => invoke('agent.setModel', { agentId, providerId, modelId }),
		setAgentMemoryModel: (agentId, providerId, modelId, off) => invoke('agent.setMemoryModel', { agentId, providerId, modelId, off }),
		resumeAgent: (agentId) => invoke('agent.resume', { agentId }),
		stopAgent: (agentId) => invoke('agent.stop', { agentId }),
		// The firm's administrator changes, archives, restores or lets go of a
		// colleague.
		editColleague: (agentId, colleague) => invoke('agent.edit', Object.assign({}, colleague, { agentId })),
		requestColleagueEdit: (agentId, colleague) => invoke('agent.requestEdit', Object.assign({}, colleague, { agentId })),
		archiveColleague: (agentId, archived) => invoke('agent.archive', { agentId, archived }),
		deleteColleague: (agentId) => invoke('agent.delete', { agentId }),
		updateAgent: (agentId, expectedRevision, settings) =>
			invoke('agent.update', Object.assign({}, settings, { agentId, expectedRevision })),
		setAgentOffice: (agentId, office) => invoke('agent.setOffice', Object.assign({}, office, { agentId })),
		runAgent: (agentId) => invoke('agent.run', { agentId }),
		cancelAgentRun: (agentId) => invoke('agent.cancelRun', { agentId }),
		markAgentRunReviewed: (agentId, runId) => invoke('agent.markReviewed', { agentId, runId }),
		assignAgentWork: (agentId, instruction) => invoke('agent.assign', { agentId, instruction }),
		// A person's words to colleagues in a thread; agents it names (@) take it up.
		postChatMessage: (projectId, chatId, text, mentions, extras) => invoke('chat.post', {
			projectId, chatId, text, mentions: mentions ?? [],
			...(extras?.quoteMessageId ? { quoteMessageId: extras.quoteMessageId } : {}),
			...(extras?.attachmentDocumentIds?.length ? { attachmentDocumentIds: extras.attachmentDocumentIds } : {}),
			...(extras?.notChecked ? { notChecked: extras.notChecked } : {}),
			...(extras?.askOptions?.length ? { askOptions: extras.askOptions } : {}),
			...(extras?.answerOption ? { answerOption: extras.answerOption } : {}),
			...(extras?.place?.documentId ? { place: extras.place } : {}),
		}),
		// A thread's closing with its conclusion, and its reopening.
		closeChatThread: (projectId, chatId, conclusion) => invoke('chat.closeThread', { projectId, chatId, conclusion }),
		reopenChatThread: (projectId, chatId) => invoke('chat.reopenThread', { projectId, chatId }),
		// A message's author changes or deletes it; anyone reacts to it.
		editChatMessage: (projectId, chatId, messageId, text) => invoke('chat.editMessage', { projectId, chatId, messageId, text }),
		deleteChatMessage: (projectId, chatId, messageId) => invoke('chat.deleteMessage', { projectId, chatId, messageId }),
		reactToChatMessage: (projectId, chatId, messageId, emoji) => invoke('chat.react', { projectId, chatId, messageId, emoji }),
		markChatReceived: (projectId, chatId) => invoke('chat.markReceived', { projectId, chatId }),
		readMarks: () => invoke('chat.readMarks', {}),
		markRead: (conversation, upTo) => invoke('chat.markRead', { conversation, upTo }),
		// A colleague's document review in a thread, and the lawyer's decision on it.
		listDocumentReviews: (projectId, chatId) => invoke('documentReview.list', { projectId, chatId }),
		getDocumentReview: (projectId, reviewId) => invoke('documentReview.get', { projectId, reviewId }),
		decideDocumentReview: (projectId, reviewId, version, decision, comment) =>
			invoke('documentReview.decide', { projectId, reviewId, version, decision, comment }),
		retryDocumentReview: (projectId, reviewId) => invoke('documentReview.retry', { projectId, reviewId }),
		listAssignments: (projectId, chatId) => invoke('documentReview.assignments', { projectId, chatId }),
		leaveOutDocumentReviewChanges: (projectId, reviewId, version, changeIds) =>
			invoke('documentReview.leaveOut', { projectId, reviewId, version, changeIds }),
		answerAssignmentDecision: (projectId, assignmentId, decisionId, answer) =>
			invoke('documentReview.answerDecision', { projectId, assignmentId, decisionId, answer }),
		decideWrittenDocument: (projectId, assignmentId, step, decision, comment) =>
			invoke('documentReview.decideWritten', { projectId, assignmentId, step, decision, comment }),
		saveDocumentReviewCopy: (projectId, reviewId, version) =>
			invoke('documentReview.saveCopy', { projectId, reviewId, version }),
		// Singularity in Word, Excel and Outlook.
		getOfficeStatus: () => invoke('office.getStatus', {}),
		connectOffice: () => invoke('office.connect', {}),
		openOfficeApp: (app) => invoke('office.openApp', { app }),
		disconnectOffice: () => invoke('office.disconnect', {}),
		listOfficeClients: () => invoke('office.listClients', {}),
		decideOfficePairing: (pairingId, allow) => invoke('office.decidePairing', { pairingId, allow }),
		removeOfficeClient: (clientId) => invoke('office.removeClient', { clientId }),
		describeOfficeFile: (location) => invoke('office.describeFile', { location: location ?? '' }),
		fileIntoMatter: (matterId, name, base64) => invoke('office.fileIntoMatter', { matterId, name, base64 }),
		readDocumentContent: (matterId, documentId) => invoke('office.documentContent', { matterId, documentId }),
		readEmailDraft: (matterId, documentId) => invoke('office.emailDraft', { matterId, documentId }),
		openOfficeDocument: (matterId, documentId) => invoke('office.openDocument', { matterId, documentId }),
)js"
R"js(		// A Matter's correspondence.
		suggestLetterMatter: (message) => invoke('correspondence.suggest', { message }),
		fileLetter: (matterId, message, evidence, from, by) =>
			invoke('correspondence.file', { matterId, message, evidence: evidence ?? [], from: from ?? 'outlook', by: by ?? 'person' }),
		moveLetter: (matterId, letterId, toMatterId) => invoke('correspondence.move', { matterId, letterId, toMatterId }),
		unfileLetter: (matterId, letterId) => invoke('correspondence.unfile', { matterId, letterId }),
		listLetters: (matterId) => invoke('correspondence.list', { matterId }),
		// A reply to a filed letter, opened in the member's mail app to send.
		draftLetterReply: (matterId, letterId, text, attachmentDocumentIds) => invoke('correspondence.draftReply', attachmentDocumentIds?.length > 0 ? { matterId, letterId, text, attachmentDocumentIds } : { matterId, letterId, text }),
		recordTime: (projectId, entry) => invoke('project.recordTime', { ...entry, projectId }),
		updateTime: (projectId, entryId, change) => invoke('project.updateTime', { ...change, projectId, entryId }),
		removeTime: (projectId, entryId) => invoke('project.removeTime', { projectId, entryId }),
		listTime: (projectId) => invoke('project.listTime', { projectId }),
		saveLetterAttachment: (matterId, letterId, providerId, base64) =>
			invoke('correspondence.saveAttachment', { matterId, letterId, providerId, base64 }),
		revealInOffice: (matterId, documentId, place) => invoke('office.reveal', {
			matterId, documentId, section: place?.section ?? '', heading: place?.heading ?? '', quote: place?.quote ?? '',
		}),
		withdrawAgentWork: (agentId, assignmentId) => invoke('agent.withdrawAssignment', { agentId, assignmentId }),
		previewAgentSetup: (projectId, modelPolicy) => invoke('agent.previewSetup', { projectId, modelPolicy }),
		chooseAgentFolder: (projectId, startPath) => invoke('agent.chooseFolder', { projectId, startPath }),
		openAgentFile: (agentId, relativePath) => invoke('agent.openFile', { agentId, relativePath }),
		revealAgentFile: (agentId, relativePath) => invoke('agent.revealFile', { agentId, relativePath }),
		listAiApps: () => invoke('agent.listAiApps', {}),
		connectAiApp: (label, projectIds, mayPauseAgents) => invoke('agent.connectAiApp', { label, projectIds, mayPauseAgents }),
		renewAiAppKey: (aiAppId) => invoke('agent.renewAiAppKey', { aiAppId }),
		disconnectAiApp: (aiAppId) => invoke('agent.disconnectAiApp', { aiAppId }),
		listConsiliumConfigurations: (projectId) => invoke('consilium.list', { projectId }),
		createConsiliumConfiguration: (projectId, name, objective) =>
			invoke('consilium.create', { projectId, name, objective }),
		updateConsiliumConfiguration: (projectId, configurationId, expectedRevision, configuration) =>
			invoke('consilium.update', { projectId, configurationId, expectedRevision, ...configuration }),
		duplicateConsiliumConfiguration: (projectId, configurationId) =>
			invoke('consilium.duplicate', { projectId, configurationId }),
		importConsiliumConfiguration: (projectId) => invoke('consilium.import', { projectId }),
		exportConsiliumConfiguration: (projectId, configurationId) =>
			invoke('consilium.export', { projectId, configurationId }),
    // Project Data - .lm. getProjectDataStatus is a cheap presence/health
    // read; setUpProjectData runs the explicit, recoverable migration once a
    // folder is chosen (idempotent after that). Wire methods use project.*;
    // projectId is the Matter's own stable id - the Matter/Project
    // implementation is the same domain.
    getProjectDataStatus: (projectId) => invoke('project.getDataStatus', { projectId }),
    setUpProjectData: (projectId, startEmpty = false) => invoke('project.setUpData', { projectId, startEmpty }),
    revealProjectData: (projectId) => invoke('project.revealData', { projectId }),
    // Resolves "" (not a rejection) if the destination-folder dialog is
    // cancelled, matching chooseMatterFolder's cancel convention.
    prepareProjectDataCleanCopy: (projectId) => invoke('project.prepareCleanCopy', { projectId }),
    removeProjectData: (projectId) => invoke('project.removeData', { projectId }),
    // Changes this Project refused because someone else had changed the same
    // thing first - visible in every area, not only Project Context.
    listProjectConflicts: (projectId) => invoke('project.listConflicts', { projectId }),
    resolveProjectConflict: (projectId, conflictId, resolution) =>
      invoke('project.resolveConflict', { projectId, conflictId, resolution }),
    getSharedActivity: (projectId) => invoke('project.sharedActivity', { projectId }),
    getFirmTeam: () => invoke('project.firmTeam', {}),
    // Who sits in each of the firm's seats, as its record says.
    listSeatProfiles: () => invoke('organisation.seatProfiles', {}),
    saveSeatProfile: (profile) => invoke('organisation.saveSeatProfile', profile),
    resolveOverwrittenChange: (projectId, changeId, choice) =>
      invoke('project.resolveOverwrittenChange', { projectId, changeId, choice }),
    confirmComputer: (projectId, actorId, deviceId) => invoke('project.confirmComputer', { projectId, actorId, deviceId }),
    revokeComputer: (projectId, actorId, deviceId) => invoke('project.revokeComputer', { projectId, actorId, deviceId }),
    // Persists this Project's explicit AI Setup differences from the Node
    // default (Phase 06) - an empty overrides array is "Reset to Singularity
    // default". See matter_bridge::save_ai_setup().
    saveProjectAiSetup: (projectId, overrides) => invoke('project.saveAiSetup', { projectId, overrides }),
    // Project discovery/restoration - the real, actually-discovered/restored
    // Projects in the active local workspace, scanning for .lm the same way
    // Singularity recognizes a Project at launch. Resolves { projects: [] }
    // rather than rejecting when no local workspace is configured yet, since
    // this backs a lightweight shell entry point (the command palette), not an
    // onboarding flow.
    discoverProjects: () => invoke('project.discover'),
    // A Matter discovery found in the lawyer's own folders, opened here.
    openDiscoveredMatter: (path) => invoke('matter.openDiscovered', { path }),
    // The firm's coordination: the token is handed over once, kept in the
    // credential store, and never returned.
    useFirmFolder: (folder) => invoke('coordination.useFirmFolder', { folder }),
    getCoordination: () => invoke('coordination.status', {}),
    shareMatterWithFirm: (projectId) => invoke('coordination.shareMatter', { projectId }),
    getMatterInvitation: (projectId) => invoke('coordination.invitation', { projectId }),
    joinFirmMatter: (invitation) => invoke('coordination.join', { invitation }),
    acceptMatterKey: (matterId, deviceId) => invoke('coordination.acceptKey', { matterId, deviceId }),
    // The firm's computers, checked by a number both show, and a Matter given
    // to a lawyer of the firm by choosing them.
    getFirmComputers: () => invoke('coordination.firmComputers', {}),
    checkFirmComputer: (deviceId) => invoke('coordination.checkComputer', { deviceId }),
    getComputerCheck: (deviceId) => invoke('coordination.computerCheck', { deviceId }),
    keepFirmComputer: (deviceId) => invoke('coordination.keepComputer', { deviceId }),
    refuseFirmComputer: (deviceId) => invoke('coordination.refuseComputer', { deviceId }),
    endComputerCheck: (deviceId) => invoke('coordination.endComputerCheck', { deviceId }),
    giveMatterTo: (projectId, actorId) => invoke('coordination.giveMatter', { projectId, actorId }),
    getMatterOffers: () => invoke('coordination.offers', {}),
    acceptMatterOffer: (matterId, fromDeviceId) => invoke('coordination.acceptOffer', { matterId, fromDeviceId }),
    declineMatterOffer: (matterId, fromDeviceId) => invoke('coordination.declineOffer', { matterId, fromDeviceId }),
    // A personal conversation of two members: opened, and every one held here.
    openPersonalChat: (otherActorId) => invoke('chat.openPersonal', { otherActorId }),
    listPersonalChats: () => invoke('chat.listPersonal', {}),
    clearConversation: (chatId) => invoke('chat.clearConversation', { chatId }),
    restoreConversation: (chatId, clearedBefore) => invoke('chat.restoreConversation', { chatId, clearedBefore }),
    listClearedConversations: () => invoke('chat.clearedConversations', {}),
    // The icons of the sites web sources come from, asked of each site.
    sourceIcons: (sites) => invoke('chat.sourceIcons', { sites }),
    // A member's Stop in a conversation: its colleagues end the work given there.
    stopColleagues: (chatId) => invoke('chat.stopColleagues', { chatId }),
    // Selected IDs are a local privacy control; document content is still
    // retrieved and injected server-side (see build_document_context), so the
    // renderer never receives or resends extracted source text.
    runMatterTask: (matterId, prompt, providerId, modelId, promptOptions = {}) =>
      invoke('matter.runTask', { ...promptOptions, matterId, prompt, providerId, modelId }),
    // Master Spec Sec.54 "What leaves this device?" - identical params shape
    // to runMatterTask, so a preview always describes the exact request a
    // same-params runMatterTask call would send. Makes no provider/network
    // call and persists nothing - see preview_outbound_request().
    previewOutboundRequest: (matterId, prompt, providerId, modelId, promptOptions = {}) =>
      invoke('matter.previewOutboundRequest', { ...promptOptions, matterId, prompt, providerId, modelId }),
    // Phase 11 (Privacy - outbound Guard and operation detail) "Reveal
    // original locally" - an on-demand, single-occurrence lookup, never a
    // stored Restoration Map. `previewToken` must be the opaque value
    // previewOutboundRequest() returned alongside this occurrenceId - it
    // names the exact ephemeral native preview session (PapinianGuard/
    // PreviewSession) reveal answers from, so a stale one (request/source/
    // provider/model/policy state changed since) fails closed instead of
    // resolving against whatever the current state now contains. See
    // reveal_protected_occurrence().
)js"
R"js(    revealProtectedOccurrence: (matterId, occurrenceId, previewToken) =>
      invoke('matter.revealProtectedOccurrence', { matterId, occurrenceId, previewToken }),
    // Explicit, immediate teardown of one ephemeral preview session (Phase
    // 11) - the normal way its life ends, not TTL expiry. Call this the
    // moment a displayed preview stops being "the current one": the review
    // sheet closes/cancels, or the prompt/source selection that produced it
    // changes. See close_outbound_preview().
    closeOutboundPreview: (matterId, previewToken) =>
      invoke('matter.closeOutboundPreview', { matterId, previewToken }),
    // Omitting providerId/modelId is Retry (reuse the original task's own
    // model); passing both is Run with another model - see
    // retry_matter_task() in native_bridge.cpp.
    retryMatterTask: (matterId, taskId, providerId, modelId) =>
      invoke('matter.retryTask', { matterId, taskId, providerId, modelId }),
    // Phase 27's "Edit and continue" / Message revision - never rewrites
    // `taskId`'s own historical prompt/response, only appends a new attempt
    // in its retry chain. `prompt` omitted reuses the original text (a pure
    // source-policy revision); `sourcePolicyMode`/`selectedDocumentIds`
    // omitted reuses the original selection - see revise_task().
    reviseMatterTask: (matterId, taskId, prompt, sourcePolicyMode, selectedDocumentIds) =>
      invoke('matter.reviseTask', { matterId, taskId, prompt, sourcePolicyMode, selectedDocumentIds }),
    // Master Spec Sec.68 (Consilium) - `targets` must name 2+ distinct
    // connected models; each independently receives the same prompt/preset/
    // sources, exactly like runMatterTask - see run_consilium().
    // Preferred: run a SAVED Consilium Configuration, which is the authoritative
    // execution plan - its participants (with their AI Setup roles and model
    // overrides), stage order and mode, objective and source policy all govern
    // the run. `prompt` is optional and only overrides the saved objective.
    runConsiliumConfiguration: (matterId, configurationId, promptOptions = {}) =>
      invoke('matter.runConsilium', { ...promptOptions, matterId, configurationId }),
    // Legacy fan-out: N models, one prompt, no participant identity and no
    // cross-visibility. Kept working for callers that have no Configuration.
    runConsilium: (matterId, prompt, targets, promptOptions = {}) =>
      invoke('matter.runConsilium', { ...promptOptions, matterId, prompt, targets }),
    // "Synthesize with..." - runs one more real task over a Consilium
    // group's own already-persisted member answers through the chosen
    // model - see synthesize_consilium().
    synthesizeConsilium: (matterId, groupId, providerId, modelId) =>
      invoke('matter.synthesizeConsilium', { matterId, groupId, providerId, modelId }),
    // Master Spec Sec.69 (Completor) - the backend's own real workflow
    // registry, so the renderer never invents or edits a workflow.
    listWorkflows: () => invoke('matter.listWorkflows', {}),
    // Runs every step of the chosen workflow in order - see run_workflow().
    // Never a caller-supplied providerId/modelId: each step resolves the
    // same effective Node -> Project -> Chat Reasoning assignment an
    // ordinary Chat send already uses when no explicit override exists.
    // chatId is optional: when a real Chat started this run, every one of
    // its own step tasks records that Chat's id (Phase 26's own workflow/
    // operation linkage), so Chat can group the run as one operation
    // instead of unrelated Project tasks.
    runWorkflow: (matterId, workflowId, workflowVersion, selectedDocumentIds, chatId) =>
      invoke('matter.runWorkflow', { matterId, workflowId, workflowVersion, selectedDocumentIds, chatId }),
    // Local Markdown/plain-text export (Master Spec Sec.24) - the renderer
    // sends only the already-local sourceId/format, never file content or a
    // filesystem path; the native Save dialog and the write itself both stay
    // inside the backend. Resolves false, not a rejection, if the user
    // cancels the dialog.
    exportMatterText: (matterId, sourceType, sourceId, format) =>
      invoke('matter.exportText', { matterId, sourceType, sourceId, format }),
    // A .docx or .xlsx the interface built from a result, saved where the
    // person chooses. Resolves false when the dialog is cancelled.
    saveGeneratedFile: (name, extension, contentBase64) =>
      invoke('dialog.saveGeneratedFile', { name, extension, contentBase64 }),
    // Phase 10 (Privacy - entities and Project coverage) - Project coverage,
    // documents needing attention and the Entity Registry review surface.
    // Real local state only: see Bridge/PrivacyBridge/PrivacyBridge.cpp -
    // never a request-scoped Restoration Map (Phase 11's separate contract).
    getPrivacyOverview: (projectId) => invoke('privacy.getOverview', { projectId }),
    getPrivacyCoverage: (projectId) => invoke('privacy.getCoverage', { projectId }),
    listPrivacyAttentionDocuments: (projectId) => invoke('privacy.listAttentionDocuments', { projectId }),
    listPrivacyEntities: (projectId) => invoke('privacy.listEntities', { projectId }),
    getPrivacyEntityDetail: (projectId, entityId) => invoke('privacy.getEntityDetail', { projectId, entityId }),
    resolvePrivacyAmbiguity: (projectId, ambiguityId, decision) =>
      invoke('privacy.resolveAmbiguity', { projectId, ambiguityId, decision }),
    splitPrivacyEntityAlias: (projectId, entityId, alias) =>
      invoke('privacy.splitEntityAlias', { projectId, entityId, alias }),
    addPrivacyEntityAlias: (projectId, entityId, alias) =>
      invoke('privacy.addEntityAlias', { projectId, entityId, alias }),
    removePrivacyEntityAlias: (projectId, entityId, alias) =>
      invoke('privacy.removeEntityAlias', { projectId, entityId, alias }),
    // What an operation's protected requests sent, beside the Project's own
    // text: re-derived locally, never a kept restoration mapping.
    getPassageDisclosure: (projectId, operationId, documentId, chunkId) =>
      invoke('privacy.getPassageDisclosure', { projectId, operationId, documentId, chunkId }),
    getOperationDisclosure: (projectId, operationId) =>
      invoke('privacy.getOperationDisclosure', { projectId, operationId }),
    // Phase 12 (Context - canonical data model and authority) - canonical
    // Project Context items and their distinct Pending suggestion queue.
    // See Bridge/ProjectContextBridge/ProjectContextBridge.cpp: acceptance/
    // dismissal are always an explicit human decision the backend attributes
    // and persists, never a silent overwrite of a user-authored/confirmed
    // item.
    listContextItems: (projectId) => invoke('context.listItems', { projectId }),
    listContextSuggestions: (projectId) => invoke('context.listSuggestions', { projectId }),
    searchContextItems: (projectId, query) => invoke('context.searchItems', { projectId, query }),
    getContextPageState: (projectId) => invoke('context.getPageState', { projectId }),
    runAutomaticContextUpdate: (projectId) => invoke('context.runAutomaticUpdate', { projectId }),
    acceptContextSuggestion: (projectId, suggestionId) =>
      invoke('context.acceptSuggestion', { projectId, suggestionId }),
    dismissContextSuggestion: (projectId, suggestionId) =>
      invoke('context.dismissSuggestion', { projectId, suggestionId }),
    // Phase 13 (Context - editing, provenance and entities) - direct human
    // create/edit/delete on a canonical Context item.
    // Talking to the Project's memory, on a local model: nothing leaves this Mac.
    memoryChat: (projectId, message) => invoke('context.memoryChat', { projectId, message }),
    memoryStatus: projectId => invoke('context.memoryStatus', { projectId }),
    memoryReadDocuments: (projectId, everything) => invoke('context.memoryReadDocuments', { projectId, everything }),
    createContextItem: (projectId, section, label, value) =>
      invoke('context.createItem', { projectId, section, label, value }),
    // Phase 28 criterion 40 - promotes one completed Chat result into
    // canonical Context as its own explicit action, distinct from a direct
    // manual entry above.
    createContextItemFromChatResult: (projectId, section, label, value, matterTaskId) =>
      invoke('context.createItemFromChatResult', { projectId, section, label, value, matterTaskId }),
    updateContextItem: (projectId, itemId, value, baseItem) =>
      invoke('context.updateItem', { projectId, itemId, value, baseItem }),
    deleteContextItem: (projectId, itemId) =>
      invoke('context.deleteItem', { projectId, itemId }),
    // A stable reference to a Project Entity Registry entry (Privacy/Types/
    // ProjectEntity.hpp) - never a second identity store, see Bridge/
    // ProjectContextBridge/ProjectContextBridge.cpp.
    linkContextItemEntity: (projectId, itemId, entityId) =>
      invoke('context.linkItemEntity', { projectId, itemId, entityId }),
    unlinkContextItemEntity: (projectId, itemId) =>
      invoke('context.unlinkItemEntity', { projectId, itemId }),
	updateContextItemControls: (projectId, itemId, controls) =>
	  invoke('context.updateItemControls', { projectId, itemId, ...controls }),
	listContextMergeConflicts: (projectId) => invoke('context.listMergeConflicts', { projectId }),
	resolveContextMergeConflict: (projectId, conflictId, resolution, editedValue) =>
	  invoke('context.resolveMergeConflict', { projectId, conflictId, resolution, editedValue }),
    getSystemPermissions: () => invoke('system.permissions'),
    askSystemPermission: (key) => invoke('system.askPermission', { key }),
    openSystemPermissionSettings: (key) => invoke('system.openPermissionSettings', { key }),
    openExternal: (url) => invoke('system.openExternal', { url }),
    on: (eventName, listener) => {
      const wrapped = (event) => listener(event.detail);
      events.addEventListener(eventName, wrapped);
      return () => events.removeEventListener(eventName, wrapped);
    },
  });

  window.addEventListener('__native_event', (event) => {
    const detail = event.detail;
    if (detail && typeof detail.name === 'string') {
      events.dispatchEvent(new CustomEvent(detail.name, { detail: detail.payload }));
    }
  });

  Object.defineProperty(window, 'nativeApp', {
    configurable: false,
    enumerable: true,
    writable: false,
    value: api,
  });

  // Compatible with Singularity's original Electron preload contract.
  Object.defineProperty(window, 'singularity', {
    configurable: false,
    enumerable: true,
    writable: false,
    value: Object.freeze({
      platform: api.platform,
      chooseFolder: api.chooseFolder,
    }),
  });

  // "page": the page carries the window's caption (Windows); "integrated":
  // the window keeps its native controls over or above the page.
  const markPlatform = () => {
    document.documentElement?.setAttribute('data-native-platform', api.platform);
    document.documentElement?.setAttribute('data-native-window-frame', api.windowFrame);
  };
  if (document.documentElement) {
    markPlatform();
  } else {
    document.addEventListener('DOMContentLoaded', markPlatform, { once: true });
  }

  const openExternal = (value) => {
    try {
      const url = new URL(value, window.location.href);
      if (url.protocol === 'https:') {
        void api.openExternal(url.href).catch(console.error);
        return true;
      }
    } catch (_) {}
    return false;
  };

  document.addEventListener('click', (event) => {
    const anchor = event.target instanceof Element
      ? event.target.closest('a[href]')
      : null;
    if (!anchor || !anchor.target || anchor.target.toLowerCase() === '_self') {
      return;
    }
    if (openExternal(anchor.href)) {
      event.preventDefault();
    }
  }, true);

  const originalOpen = window.open.bind(window);
  window.open = (url, target, features) => {
    if (typeof url === 'string' && openExternal(url)) {
      return null;
    }
    return originalOpen(url, target, features);
  };

  queueMicrotask(() => {
    void api.getRuntimeInfo()
      .then((payload) => {
        events.dispatchEvent(new CustomEvent('backend.ready', { detail: payload }));
      })
      .catch(console.error);
  });
})();
)js";
}

} // namespace

// What a local model replies when asked to name a new chat (ChatExecution.hpp).
// The model already loaded on this Mac is asked if there is one, otherwise the
// first installed generative model, loaded through the same governed path every
// local request takes. Papinian's own extraction encoder cannot write and is
// never a candidate. A chat that itself runs on the local model is named by its
// own answer instead: two generations are never set against each other for a
// title. "" whenever there is nothing to ask.
std::string quick_chat_title_reply(const std::string &project_id, const std::string &chat_id,
                                   const std::string &first_message) {
  chat::OperationSetupRequest setupRequest;
  setupRequest.project_id = project_id;
  setupRequest.chat_id = chat_id;
  const chat::OperationSetup setup = chat::resolve_operation_setup(setupRequest);
  if (setup.primary_model && setup.primary_model->provider_id == "local") return {};

  const std::optional<std::string> model = routing::preferred_local_generative_model();
  if (!model) return {};

  routing::InferenceRequest request;
  request.call_purpose = routing::observability::CallPurpose::ContextAutomation;
  request.model_target = routing::ModelTarget{"local", *model};
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    request.workspace_id = platform::load_app_settings().local_node_setup.active_workspace_id;
  }
  // Room for a model that thinks aloud before it answers; "/no_think" asks the
  // ones that honour it (Qwen3) to answer at once. What is kept of the reply is
  // the Chat domain's own to decide (chat::chat_title_from_reply).
  request.maximum_output_tokens = 320;
  request.prompt = "Name this conversation in three to six words, in the language of the message: what the work "
      "is about, as briefly as it can be said. Write it as a sentence is written, with a capital only for the "
      "first word and for names, and keep every abbreviation and name exactly as the message writes it. Name "
      "the subject itself, never the conversation: do not begin with a word like Conversation, Discussion or "
      "Question. Reply with the name only: no quotation marks, no full stop, nothing else.\n\nFirst message:\n"
      + first_message.substr(0, 1200) + "\n\n/no_think";
  // Diagnostic trace only (SINGULARITY_TRACE_FILE): that it ran, on what, how long
  // it took and how it ended - never the message or the name.
  platform::trace::Span span("title", "quick_chat_title", {{"model", *model}});
  try {
    const routing::InferenceResult result = routing::execute(request);
    span.add("outcome", "ok");
    span.add("replyBytes", static_cast<long long>(result.text.size()));
    // Naming a chat is local work like any other, and Usage counts it as such.
    usage::LocalWork named;
    named.project_id = project_id;
    named.operation_type = usage::kLocalWorkChatNaming;
    named.label = "Chat named";
    named.model_id = *model;
    named.input_tokens = result.usage.input_tokens.value_or(0);
    named.output_tokens = result.usage.output_tokens.value_or(0);
    named.usage = result.usage;
    named.protected_api_applied = result.protected_api_applied;
    named.privacy_metadata = result.privacy_metadata;
    usage::record_local_work(named);
    return result.text;
  } catch (const std::exception &error) {
    span.add("outcome", "failed");
    span.add("reason", std::string(error.what()).substr(0, 160));
    throw;
  }
}

// The pick of a request's documents from a list (Context/SourcePicker), on
// the model that will run the request - a provider's model reached through
// Papinian Guard with the Project's entities protected, exactly as the
// request itself will be. A short answer: handles and a flag.
routing::matter_request_execution::SourcePickReply pick_sources(
    const routing::matter_request_execution::SourcePickCall &call) {
  routing::InferenceRequest request;
  request.call_purpose = routing::observability::CallPurpose::ContextAutomation;
  request.model_target = call.model;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    request.workspace_id = platform::load_app_settings().local_node_setup.active_workspace_id;
  }
  if (call.model.provider_id == "local") {
    const auto window = routing::estimate_model_context_window_tokens(call.model);
    request.maximum_output_tokens = window ? std::max<std::uint64_t>(1024, *window / 4) : 2048;
  } else {
    request.maximum_output_tokens = 8192;
  }
  request.prompt = call.prompt;
  request.research_mode = research::ResearchMode::Off;
  request.research_final_output_contract = call.contract;
  // Under the run's own policy, never the live setting: a re-run that pinned
  // a stronger protection keeps it for the pick too.
  if (call.protected_api_policy) request.protected_api_policy_override = call.protected_api_policy;
  if (call.model.provider_id != "local") request.project_entity_hints = context_actions::memory_protection_hints(call.project_id);
  platform::trace::Span span("evidence", "source_pick_call", {{"model", call.model.model_id}});
  const routing::InferenceResult result = routing::execute(request);
  span.add("replyBytes", static_cast<long long>(result.text.size()));
  return {result.text, result.usage};
}

// What the Project's memory keeper (Context/MemoryKeeper) needs from the
// application: which model keeps a Project's memory, one generation on it, and
// the readable text of a document. The keeper itself knows nothing of the
// local runtime, of providers or of the document store.
memory_keeper::Dependencies memory_keeper_dependencies() {
  memory_keeper::Dependencies dependencies;
  // The Context model the lawyer assigned (AI Setup) - a provider's model is
  // reached through Papinian Guard like every request - or, with none
  // assigned, a local model this Mac has.
  dependencies.choose_model = [](const std::string &project_id) -> std::optional<memory_keeper::Keeper> {
    try {
      if (const auto assigned = context_actions::memory_model(project_id)) {
        return memory_keeper::Keeper{assigned->target.provider_id, assigned->target.model_id, assigned->execution_kind};
      }
    } catch (const std::exception &) {
    }
    const std::optional<std::string> local = routing::preferred_local_generative_model();
    if (!local) return std::nullopt;
    return memory_keeper::Keeper{"local", *local, "local"};
  };
  dependencies.generate = [](const std::string &project_id, const memory_keeper::Keeper &keeper,
                             const std::string &prompt) {
    routing::InferenceRequest request;
    request.call_purpose = routing::observability::CallPurpose::ContextAutomation;
    request.model_target = routing::ModelTarget{keeper.provider_id, keeper.model_id};
    {
      const persistence::PersistenceLock lock(persistence::persistence_mutex());
      request.workspace_id = platform::load_app_settings().local_node_setup.active_workspace_id;
    }
    // No ceiling on the answer. A provider left without max_tokens applies a
    // default of its own and reports "length" - so a provider's model is asked
    // for the most it can write; a local model, for as much as its window leaves it.
    if (keeper.execution_kind == "local") {
      const auto window = routing::estimate_model_context_window_tokens(*request.model_target);
      request.maximum_output_tokens = window ? std::max<std::uint64_t>(1024, *window / 2) : 4096;
    } else {
      request.maximum_output_tokens = 32768;
    }
    request.prompt = prompt;
    // The Project's entities, so Papinian Guard replaces them before a
    // provider's model sees the memory or a document.
    if (keeper.execution_kind == "external") request.project_entity_hints = context_actions::memory_protection_hints(project_id);
    const routing::InferenceResult result = routing::execute(request);
    return memory_keeper::Generated{result.text, result.usage.input_tokens.value_or(0),
                                    result.usage.output_tokens.value_or(0), result.usage,
                                    result.protected_api_applied, result.privacy_metadata};
  };
  // A document's passages in their own order, from its beginning: who, what
  // and when stand at the head of a legal document.
  dependencies.read_document = [](const std::string &project_id, const std::string &document_id,
                                  std::size_t maximum_bytes) {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto workspace = node::require_active_workspace_path();
    const auto projects = matters::load_matters(workspace);
    const auto project = std::find_if(projects.begin(), projects.end(),
        [&](const matters::Matter &candidate) { return candidate.id == project_id; });
    if (project == projects.end()) return std::string();
    auto chunks = matters::retrieve_document_context(workspace, *project, {}, {document_id}, nullptr,
        matters::SelectedSourceDelivery::EveryPassage);
    std::stable_sort(chunks.begin(), chunks.end(), [](const auto &left, const auto &right) {
      return left.chunk.source_start < right.chunk.source_start;
    });
    std::string text;
    for (const auto &entry : chunks) {
      if (text.size() >= maximum_bytes) break;
      text += entry.chunk.text;
      text += "\n\n";
    }
    return memory_keeper::bounded_text(text, maximum_bytes);
  };
  return dependencies;
}

class NativeBridge::Impl final {
public:
  explicit Impl(app::AppWindow &window, bool start_background_services)
      : window_(window), executor_(default_worker_count()), runs_(kRunThreads), controls_(kControlThreads),
        downloads_(1) {
    // Every picker and Save dialog opens over the window that asked for it.
    dialogs_.set_parent_window([this] { return window_.dialog_parent(); });
    register_default_handlers();
    // What agents need from the rest of Singularity; the agents themselves
    // check their rooms only in the window (start_background_services). Each
    // colleague shows in every Matter this computer carries with the firm's
    // other computers (Agent/AgentCard).
    agents::RunnerDependencies runner = agents::production_runner_dependencies();
    // The names that identify each Matter's client here: a colleague's note that
    // carries one is that client's, whatever a model labelled it.
    runner.matter_identities = []() {
      std::vector<agents::MatterIdentities> table;
      for (const matters::Matter &one : matter::list())
        table.push_back({one.id, one.name, privacy::identifying_names(one)});
      return table;
    };
    runner.carried_matters = []() {
      std::vector<std::string> ids;
      for (const matter::coordination::CarriedMatter &carried : matter::coordination::carried_matters())
        ids.push_back(carried.matter_id);
      return ids;
    };
    // Only the firm's administrator hires its AI colleagues, chooses their
    // models and connects its model accounts.
    runner.may_manage_colleagues = [] { return organisation::is_administrator(); };
    // A change of a colleague an administrator asked for on another of the
    // firm's computers is taken only from one checked here, of a seat Legal
    // Machines lists as the administrator's.
    runner.administrator_computer_key = [](const std::string &device_id) {
      return matter::firm_computers::administrator_computer_key(device_id);
    };
    agents::install_runner_dependencies(std::move(runner));
    // What every computer of the firm holds alike travels in the firm's record
    // and in each Matter's own folder, sealed between the firm's computers:
    // the colleagues' settings and memory, and each member's read marks. The
    // firm's record is made once a minute at most; the Matters' folders are
    // read again every ten seconds at most.
    {
      static std::mutex placesMutex;
      static std::optional<std::filesystem::path> firmFolder;
      static std::chrono::steady_clock::time_point firmAt {};
      static std::map<std::string, std::filesystem::path> matterFolders;
      static std::chrono::steady_clock::time_point mattersAt {};
      const auto firmRecord = []() -> std::optional<std::filesystem::path> {
        const std::lock_guard<std::mutex> lock(placesMutex);
        const auto now = std::chrono::steady_clock::now();
        if (firmFolder.has_value() == false || now - firmAt >= std::chrono::minutes(1)) {
          try {
            firmFolder = matter::coordination::firm_channel().folder;
          } catch (const std::exception &) {
          }
          firmAt = now;
        }
        return firmFolder;
      };
      const auto folders = []() {
        const std::lock_guard<std::mutex> lock(placesMutex);
        const auto now = std::chrono::steady_clock::now();
        if (mattersAt == std::chrono::steady_clock::time_point {} || now - mattersAt >= std::chrono::seconds(10)) {
          std::map<std::string, std::filesystem::path> found;
          try {
            for (const matters::Matter &entry : matters::load_matters(node::require_active_workspace_path())) {
              if (entry.directory_path.has_value() == true && entry.directory_path->empty() == false && matter::is_desk(entry) == false)
                found[entry.id] = std::filesystem::u8path(*entry.directory_path);
            }
            matterFolders = std::move(found);
          } catch (const std::exception &) {
          }
          mattersAt = now;
        }
        return matterFolders;
      };
      agents::SharedPlaces places;
      places.firm = firmRecord;
      places.matter = [folders](const std::string &matter_id) -> std::optional<std::filesystem::path> {
        const std::map<std::string, std::filesystem::path> known = folders();
        const auto found = known.find(matter_id);
        if (found == known.end()) return std::nullopt;
        return found->second;
      };
      places.matters = [folders]() {
        std::vector<std::filesystem::path> list;
        for (const auto &[id, folder] : folders()) list.push_back(folder);
        return list;
      };
      places.written = []() { matter::coordination::wake(); };
      places.device_id = []() { return platform::current_device_id(); };
      agents::install_shared_places(places);
      chat::ReadMarksPlace marks;
      marks.firm = firmRecord;
      marks.device_id = []() { return platform::current_device_id(); };
      marks.written = []() { matter::coordination::wake(); };
      chat::install_read_marks_place(marks);
      // Who sits in each of the firm's seats, in the same record.
      organisation::ProfilePlace profiles;
      profiles.firm = firmRecord;
      profiles.device_id = []() { return platform::current_device_id(); };
      profiles.now = []() { return agents::agent_timestamp_now(); };
      profiles.is_photo = [](const std::string &photo) { return matter::is_member_photo(photo); };
      profiles.written = [](const std::filesystem::path &) { matter::coordination::wake(); };
      organisation::install_profile_place(profiles);
    }
    providers::set_may_connect_accounts([] { return organisation::is_administrator(); });
    agents::install_ai_app_dependencies(agents::production_ai_app_dependencies());
    // A request in the thread of an agent set to work only on this computer is
    // that agent's work, and stays here.
    chat::set_agent_works_only_here(agents::agent_works_only_here);
    // A thread deleted here ends the colleagues' work given in it.
    chat::set_deleted_listener([](const std::string &, const std::string &chat_id) {
      (void)agents::withdraw_work_in_deleted_chat(chat_id);
    });
    // The composer says where kept-here work really runs.
    chat::set_kept_here_resolver(chat::kept_here_model);
    // A Matter shared between computers is kept by its folder watch; the record
    // of the person here in it says their place in the firm.
    matter::set_own_office_source([] { return organisation::own_office(); });
    // And the photo they show the firm.
    matter::set_own_photo_source([] { return organisation::own_photo(); });
    // Who is of one firm is told by the organisation's id.
    matter::set_own_organisation_source([] { return matter::coordination::signed_in_organisation_id(); });
    // The firm's seal on its Matters: this computer seals what it makes for the
    // organisation it is signed in to; a seal by a colleague's computer holds
    // as the person here decided about that computer (Tool/DeviceTrust); a
    // Matter the firm's exchange carries here is the firm's; and a Matter
    // listed here keeps its seal as it opens (matter::keep_seal).
    matter::set_seal_trust([](const std::string &actor_id, const std::string &public_key) {
      switch (tools::device_trust(actor_id, public_key)) {
        case tools::DeviceTrustState::ThisComputer:
        case tools::DeviceTrustState::Confirmed: return matter::SealComputerTrust::Confirmed;
        case tools::DeviceTrustState::Revoked: return matter::SealComputerTrust::Refused;
        case tools::DeviceTrustState::Unconfirmed: break;
      }
      return matter::SealComputerTrust::Unconfirmed;
    });
    matter::set_seal_carrier(matter::coordination::carries);
    matter::set_matter_seal_hooks(matter::MatterSealHooks{
        [](const std::filesystem::path &folder, const std::string &matter_id) { seal_with_this_computer(folder, matter_id); },
        [](const std::filesystem::path &folder, const std::string &matter_id) {
          return matter::is_firms_matter(folder, matter_id, signed_in_organisation());
        },
        [](const std::filesystem::path &folder, const std::string &matter_id) {
          return matter::may_open_matter(folder, matter_id, signed_in_organisation());
        },
        // Signed in to an organisation, a Matter sealed for another is not
        // listed here; signed in to none, every one is.
        [](const std::filesystem::path &folder, const std::string &matter_id) {
          const std::string organisation = signed_in_organisation();
          return !organisation.empty()
              && matter::matter_seal_state(folder, matter_id, organisation) == matter::MatterSealState::OtherOrganisation;
        }});
    {
      matter::SharedMatterHooks sharedHooks = matter::production_shared_matter_hooks();
      const auto recordOpened = sharedHooks.opened;
      sharedHooks.opened = [recordOpened](const std::string &project_id, const std::filesystem::path &folder) {
        try {
          const platform::ActorIdentity identity = platform::resolve_actor_identity();
          matter::keep_seal(folder, project_id, signed_in_organisation(), identity.actor_id, identity.device_id,
              workspaces::current_timestamp());
        } catch (const std::exception &) {
          // A seal that cannot be written leaves the Matter open here, unsealed.
        }
        if (recordOpened) recordOpened(project_id, folder);
      };
      matter::set_shared_matter_hooks(std::move(sharedHooks));
    }
    // And carried between the firm's computers by its coordination.
    matter::coordination::install_environment(matter::coordination::production_environment());
    matter::coordination::start();
    // The firm's computers, found through Legal Machines and checked by their
    // lawyers by a number both show. Their background runs in the window
    // only, below; quiet while no seat is signed in.
    matter::firm_computers::install_environment(matter::firm_computers::production_environment());
    // A design partner's work log lives in this computer's own records, kept
    // only while the seat's own record says design partner. A build without
    // Legal Machines' keys for work logs could never send one: it keeps none
    // from its first call, and erases what an earlier build kept.
    node::work_log::set_directory(platform::computer_records_directory() / "Work Log");
    // A Matter's chats keep their calls in the Matter's own folder, which
    // carries them to each of its computers, so any member's computer sends a
    // thread's log.
    node::work_log::set_matter_records({matter::records_folder_of, matter::records_folders, platform::resolve_actor_identity().device_id});
    if (node::work_log_share::recipients_ready() == false) node::work_log::set_kept(false);
    // A Matter other people work in keeps its data: one person's removal must
    // not erase every member's history. What the domains say changed reaches
    // the window as it happens: a page reads again what it shows, instead of
    // polling.
    platform::events::set_sink([gate = event_gate_, window = &window_, listeners = event_listeners_](
                                   const std::string &name, const Json &payload) {
      deliver_event(gate, *window, name, payload);
      deliver_to_listeners(listeners, name, payload);
    });
    matter::set_removal_guard([](const std::filesystem::path &folder) -> std::optional<std::string> {
      if (!matter::has_other_members(folder, platform::resolve_actor_identity().actor_id)) return std::nullopt;
      return std::string("Other people work in this Matter from their own computers. Removing its Workspace data "
                         "would remove it for them too, so nothing was removed.");
    });

    // Start loading Papinian's on-device model now, off the UI thread,
    // instead of on whatever real request happens to touch Protected API
    // first. Submitted through the BackgroundExecutor that runs take (it is
    // long like one, and the window's reads must not wait behind it), so it
    // is safely joined by ~BackgroundExecutor (which drains queued tasks before exiting - see
    // background_executor.cpp's worker_loop()) if the window closes while
    // it is still loading - deliberately not a detached std::thread, which
    // could otherwise still be running when this process's other static
    // objects (including the Papinian Guard singleton itself) are torn
    // down at exit.
		if (start_background_services) {
			// AI colleagues and the firm's coordination work while the window is
			// in the background: the system is asked not to slow them there
			// (macOS's App Nap, Windows' power throttling).
			(void)platform::keep_working_in_background("AI colleagues and the firm's coordination work in the background");
			// A killed earlier session can leave its local model runtime running,
			// holding the model's memory; end it before anything measures memory
			// or prepares a model.
			(void)platform::stale_runtime::reap_stale_runtimes();
			// Agents check their rooms on their own thread while the window is
			// open (Agent/AgentScheduler).
			agents::AgentScheduler::shared().start();
			// This computer says itself to the firm's others and hears their
			// checks every few seconds while the window is open.
			matter::firm_computers::start();
			// Outside AI apps are not answered: AI apps left the product.
			runs_.submit([this] {
				// The Project's memory is kept by a local model (MemoryKeeper.hpp): it
				// hears of every answered request, and reads what it has not read of
				// each Project's documents. Started first, so a chat answered while the
				// protection model is still being prepared is not lost to it.
				memory_keeper::start(memory_keeper_dependencies());
				routing::matter_request_execution::set_answered_observer(memory_keeper::chat_answered);
				{
					// Singularity closing stops a protection-model download this starts.
					const platform::HttpCancelScope downloads(package_downloads_);
					routing::prepare_papinian_model_at_launch();
				}
				privacy_actions::ensure_semantic_detection_store();
				// A new chat is named at once by a local model (ChatExecution.hpp).
				chat::enable_quick_chat_titles(quick_chat_title_reply);
				routing::matter_request_execution::set_source_picker(pick_sources);
				// With the model loaded, queue protection of every Project's
				// documents on the background protection worker, so no surface
				// has to be the one that first pays for it and no bridge worker
				// is held while it runs. Each Project is its own pass and its
				// own failure, and nothing here is an error the user sees.
				try {
					// The ids alone: queuing a pass must not replay every Project first.
					for (const std::string &id : matters::list_matter_ids(node::require_active_workspace_path())) {
						privacy_actions::request_preprocessing(id);
					}
				} catch (const std::exception &) {
				}
			});
		}
  }

  void shutdown() noexcept {
    // Once, whoever asks first: the window's close or a quit.
    if (shut_down_.exchange(true)) return;
    // Nothing new begins anywhere from here on (Platform/AppLifecycle): a run
    // the pool takes up does not start, an agent begins no check, a table or a
    // batch of changes ends between two requests, and every request bound to
    // the closing stops on its connection.
    platform::lifecycle::begin_closing();
    // Order matters: live operations are told to stop first (which wakes an
    // approval wait), so a worker inside one can actually return before the
    // executor waits for it.
    try {
      (void)singularity::chat::interrupt_live_operations();
    } catch (const std::exception &) {
    }
    // An agent's check in progress is stopped and said to be interrupted.
    agents::AgentScheduler::shared().stop();
    // The firm's computers' background, and the coordination's round in
    // progress, finish; no other begins.
    matter::firm_computers::stop();
    matter::coordination::stop();
    chat::stop_quick_chat_titles();
    routing::matter_request_execution::set_answered_observer({});
    routing::matter_request_execution::set_source_picker({});
    matter::set_removal_guard({});
    // From here on no event reaches the window, not even one still queued
    // when the window closes.
    try {
      const std::lock_guard<std::mutex> lock(event_gate_->mutex);
      event_gate_->open = false;
    } catch (...) {
    }
    platform::events::set_sink({});
    memory_keeper::stop();
    privacy_actions::stop_background_protection();
    // A model package still arriving is abandoned, never waited for: its
    // download is simply started again the next time.
    package_downloads_->cancel();
    downloads_.shutdown();
    runs_.shutdown();
    controls_.shutdown();
    executor_.shutdown();
    // Last, when no request can begin a watch any more: once this returns no
    // folder watch's callback is running.
    matter::stop_watching_all_documents();
  }

  void install() {
    window_.bind("__nativeInvoke",
                 [this](const std::string &request_id,
                        const std::string &request) {
                   handle_request(request_id, request);
                 });
    window_.initialize(bridge_script(window_.frame().caption_in_page ? "page" : "integrated"));
  }

  // The listeners of the carriers other than the window (the Office
  // connection), each with every event.
  struct EventListeners {
    std::mutex mutex;
    int next = 0;
    std::map<int, std::function<void(const std::string &, const std::string &)>> listeners;
  };

  static void deliver_to_listeners(const std::shared_ptr<EventListeners> &listeners, const std::string &name,
                                   const Json &payload) {
    std::vector<std::function<void(const std::string &, const std::string &)>> each;
    {
      const std::lock_guard<std::mutex> lock(listeners->mutex);
      if (listeners->listeners.empty()) return;
      for (const auto &entry : listeners->listeners) each.push_back(entry.second);
    }
    const std::string text = payload.dump();
    for (const auto &listener : each) {
      try {
        listener(name, text);
      } catch (...) {
      }
    }
  }

  int add_event_listener(std::function<void(const std::string &, const std::string &)> listener) {
    const std::lock_guard<std::mutex> lock(event_listeners_->mutex);
    const int handle = ++event_listeners_->next;
    event_listeners_->listeners.emplace(handle, std::move(listener));
    return handle;
  }

  void remove_event_listener(int handle) {
    const std::lock_guard<std::mutex> lock(event_listeners_->mutex);
    event_listeners_->listeners.erase(handle);
  }

  std::string page_script() const {
    return bridge_script("page");
  }

  // A call another carrier brings (NativeBridge::answer_carried_call).
  NativeBridge::CarriedAnswer answer_carried_call(const std::string &method, const std::string &json_params) {
    const auto rejected = [](const NativeError &error) {
      return NativeBridge::CarriedAnswer{1, nativeErrorJson(error).dump()};
    };
    const auto found = handlers_.find(method);
    if (found == handlers_.end() || found->second.policy != ExecutionPolicy::Worker) {
      return rejected(nativeErrorFor("native.method_not_found", "This carrier does not reach " + method + "."));
    }
    Json params;
    try {
      params = json_params.empty() ? Json::object() : Json::parse(json_params);
    } catch (const std::exception &error) {
      return rejected(nativeErrorFor("native.invalid_request", error.what()));
    }
    if (!params.is_object()) return rejected(nativeErrorFor("native.invalid_request", "Native method params must be an object."));
    if (shut_down_) return rejected(nativeErrorFor("native.handler_failed", "Singularity is closing."));
    auto promise = std::make_shared<std::promise<NativeBridge::CarriedAnswer>>();
    std::future<NativeBridge::CarriedAnswer> answered = promise->get_future();
    BackgroundExecutor &pool = holds_a_run(method) ? runs_ : is_operation_control(method) ? controls_ : executor_;
    try {
      pool.submit([promise, callback = found->second.callback, params] {
        try {
          promise->set_value(NativeBridge::CarriedAnswer{0, callback(params).dump()});
        } catch (...) {
          promise->set_value(NativeBridge::CarriedAnswer{1, nativeErrorJson(nativeErrorOf(std::current_exception())).dump()});
        }
      });
      return answered.get();
    } catch (...) {
      return rejected(nativeErrorOf(std::current_exception()));
    }
  }

  void emit(const std::string &event_name, const std::string &json_payload) {
    Json payload;
    try {
      payload = Json::parse(json_payload);
    } catch (const std::exception &error) {
      throw std::invalid_argument(std::string("Invalid event JSON: ") +
                                  error.what());
    }
    emit_json(event_name, payload);
  }

  // An event to the window's page (nativeApp.on), from any thread: it is
  // delivered on the window's own thread, after the caller has moved on.
  void emit_json(const std::string &event_name, const Json &payload) {
    deliver_event(event_gate_, window_, event_name, payload);
  }

  // Whether events may still reach the window. Closed when the bridge shuts
  // down, before the window is destroyed: an event still queued then is
  // dropped, never run against a window that is closing.
  struct EventGate {
    std::mutex mutex;
    bool open = true;
  };

  // Nothing here touches the bridge itself, so an event delivered late finds
  // only the gate, which outlives both. Every platform's dispatch only queues
  // its callback, so holding the gate while it is queued cannot deadlock.
  static void deliver_event(const std::shared_ptr<EventGate> &gate, app::AppWindow &window,
                            const std::string &event_name, const Json &payload) {
    const auto event = Json{{"name", event_name}, {"payload", payload}};
    auto script = std::string("window.dispatchEvent(new CustomEvent(") +
                  Json("__native_event").dump() +
                  ", { detail: " + event.dump() + " }));";
    const std::lock_guard<std::mutex> lock(gate->mutex);
    if (!gate->open) return;
    window.dispatch([gate, target = &window, script = std::move(script)] {
      const std::lock_guard<std::mutex> lock(gate->mutex);
      if (gate->open) target->evaluate(script);
    });
  }

	std::string invoke_worker(const std::string &method,
	                         const std::string &json_params) {
		const auto found = handlers_.find(method);
		if (found == handlers_.end()) {
			throw std::runtime_error("Unknown native method: " + method);
		}
		if (found->second.policy != ExecutionPolicy::Worker) {
			throw std::runtime_error("This native method requires the live application window.");
		}
		const auto params = json_params.empty() ? Json::object() : Json::parse(json_params);
		if (!params.is_object()) throw std::runtime_error("Native method params must be an object.");
		return found->second.callback(params).dump();
	}

private:
  void register_handler(std::string name, ExecutionPolicy policy,
                        std::function<Json(const Json &)> callback,
                        std::function<Json(const Json &)> continuation = nullptr) {
    handlers_.emplace(std::move(name),
                      Handler{policy, std::move(callback), std::move(continuation)});
  }

  void register_default_handlers() {
    register_handler("dialog.chooseFolder", ExecutionPolicy::UiThread,
                     [this](const Json &) -> Json {
                       const auto folder = dialogs_.choose_folder();
                       return folder ? Json(*folder) : Json(nullptr);
                     });

    // Phase 09's "Add external file" via an explicit file picker (as well
    // as drag-and-drop) - returns raw absolute paths only, empty (not an
    // error) if cancelled. Importing those paths into a Project is a
    // separate, explicit matter.addExternalFile call - see DocumentBridge.
    register_handler("dialog.chooseFiles", ExecutionPolicy::UiThread,
                     [this](const Json &) -> Json { return dialogs_.choose_files(); });

    register_handler("permissions.getState", ExecutionPolicy::Worker,
                     platform_bridge::get_permissions);

    register_handler("permissions.chooseFilesAndProjects", ExecutionPolicy::UiThread,
                     [this](const Json &) -> Json {
                       return platform_bridge::choose_files_and_projects(dialogs_.choose_folder());
                     });

    register_handler("permissions.declineFilesAndProjects", ExecutionPolicy::Worker,
                     platform_bridge::decline_files_and_projects);

    register_handler("permissions.allowBroaderAccess", ExecutionPolicy::Worker,
                     platform_bridge::allow_broader_access);

    register_handler("permissions.revokeBroaderAccess", ExecutionPolicy::Worker,
                     platform_bridge::revoke_broader_access);

    // What the computer itself allows, asked in onboarding (Platform/SystemPermissions).
    register_handler("system.permissions", ExecutionPolicy::Worker, platform_bridge::system_permissions);
    register_handler("system.askPermission", ExecutionPolicy::Worker, platform_bridge::ask_system_permission);
    register_handler("system.openPermissionSettings", ExecutionPolicy::Worker, platform_bridge::open_system_permission_settings);

    register_handler("system.openExternal", ExecutionPolicy::UiThread,
                     [](const Json &params) -> Json {
                       const auto url = params.at("url").get<std::string>();
                       if (!platform::open_external_https(url)) {
                         throw std::runtime_error(
                             "Only valid HTTPS URLs can be opened externally");
                       }
                       return true;
                     });

    register_handler("runtime.getInfo", ExecutionPolicy::Worker,
                     [this](const Json &) { return runtime_info(); });

    register_handler("runtime.getBuildHash", ExecutionPolicy::Worker,
                     [this](const Json &) { return build_hash(); });

    // Singularity's new versions from Legal Machines' site (Node/AppUpdate):
    // a download runs under the closing's cancel, as a model package's does.
    register_handler("update.status", ExecutionPolicy::Worker, update_bridge::status);
    register_handler("update.check", ExecutionPolicy::Worker, update_bridge::check);
    register_handler("update.download", ExecutionPolicy::Worker,
                     [this](const Json &params) {
                       const platform::HttpCancelScope downloads(package_downloads_);
                       return update_bridge::download(params);
                     });
    register_handler("update.show", ExecutionPolicy::UiThread, update_bridge::show);
    // Restart now: the version readied installs as Singularity ends, and
    // Singularity ends the ordinary way, as when its window closes.
    register_handler("update.install", ExecutionPolicy::UiThread,
                     [this](const Json &params) {
                       Json installing = update_bridge::install(params);
                       window_.close();
                       return installing;
                     });

    register_handler("onboarding.getStatus", ExecutionPolicy::Worker,
                     node_bridge::get_onboarding_status);

    // Transitions the live window from the locked onboarding size to the
    // resizable main-application size, so it must run on the UI thread like
    // the other handlers that touch native window/AppKit state.
    register_handler("onboarding.complete", ExecutionPolicy::UiThread,
                     [this](const Json &) { return complete_onboarding(); });

    register_handler("window.prepareMain", ExecutionPolicy::UiThread,
                     [this](const Json &) {
                       window_.clear_size_constraints();
                       window_.set_minimum_size(app::kMainWindowMinimumSize.width,
                                                app::kMainWindowMinimumSize.height);
                       window_.set_size(app::kMainWindowInitialSize.width,
                                        app::kMainWindowInitialSize.height);
                       return Json{{"ready", true}};
                     });

    // The page's own controls inside the strip that otherwise drags the
    // window (AppWindow::set_titlebar_controls). Touches AppKit hit-testing
    // state, so it runs on the UI thread.
    register_handler("window.setTitlebarControls", ExecutionPolicy::UiThread,
                     [this](const Json &params) {
                       const auto found = params.find("controls");
                       if (found == params.end() || !found->is_array()) {
                         throw std::invalid_argument("Expected controls: [{ x, y, width, height }]");
                       }
                       // A header has a handful of controls; a page reporting
                       // more than this is not describing a titlebar.
                       constexpr std::size_t kMaximumControls = 64;
                       if (found->size() > kMaximumControls) {
                         throw std::invalid_argument("Too many titlebar controls");
                       }
                       std::vector<app::AppWindow::TitlebarControl> controls;
                       controls.reserve(found->size());
                       for (const auto &entry : *found) {
                         if (!entry.is_object()) {
                           throw std::invalid_argument("A titlebar control is { x, y, width, height }");
                         }
                         app::AppWindow::TitlebarControl control;
                         control.x = entry.value("x", 0.0);
                         control.y = entry.value("y", 0.0);
                         control.width = entry.value("width", 0.0);
                         control.height = entry.value("height", 0.0);
                         controls.push_back(control);
                       }
                       const auto applied = controls.size();
                       window_.set_titlebar_controls(std::move(controls));
                       return Json{{"applied", applied}};
                     });

    // The window's frame for a page that draws its own header
    // (AppWindow::frame): whether the page carries the caption, and whether
    // the window is maximized, so the page's maximize button shows restore.
    register_handler("window.getFrame", ExecutionPolicy::UiThread,
                     [this](const Json &) {
                       const auto frame = window_.frame();
                       return Json{{"captionInPage", frame.caption_in_page},
                                   {"maximized", frame.maximized}};
                     });

    // A command of the window's caption, from the page's own button
    // (AppWindow::run_caption_command). Touches the native window, so it
    // runs on the UI thread.
    register_handler("window.captionCommand", ExecutionPolicy::UiThread,
                     [this](const Json &params) {
                       const auto found = params.find("command");
                       if (found == params.end() || !found->is_string()) {
                         throw std::invalid_argument("Expected command: minimize, maximize, restore or close");
                       }
                       const auto &name = found->get_ref<const std::string &>();
                       app::AppWindow::CaptionCommand command;
                       if (name == "minimize") {
                         command = app::AppWindow::CaptionCommand::Minimize;
                       } else if (name == "maximize") {
                         command = app::AppWindow::CaptionCommand::Maximize;
                       } else if (name == "restore") {
                         command = app::AppWindow::CaptionCommand::Restore;
                       } else if (name == "close") {
                         command = app::AppWindow::CaptionCommand::Close;
                       } else {
                         throw std::invalid_argument("Expected command: minimize, maximize, restore or close");
                       }
                       const bool applied = window_.run_caption_command(command);
                       return Json{{"applied", applied}, {"maximized", window_.frame().maximized}};
                     });

    // Non-secret resume marker for the seat that most recently created or
    // signed in to a Legal Machines account on this device - see
    // AccountResume in app_settings.hpp for why this is not a session
    // token.
    register_handler("onboarding.saveAccountResume", ExecutionPolicy::Worker,
                     node_bridge::save_account_resume);

    // Incremental per-device Singularity setup (Phase B): default model,
    // current step. Callers pass only the fields that changed; anything
    // omitted keeps its previously saved value. Workspaces and providers
    // are their own narrow methods below, not part of this patch.
    register_handler("onboarding.saveLocalSetup", ExecutionPolicy::Worker,
                     node_bridge::save_local_setup);

    register_handler("settings.getLegalInstructions", ExecutionPolicy::Worker,
                     node_bridge::get_legal_instructions);
    // The member's name is kept on this computer and in the firm's record,
    // never at Legal Machines.
    register_handler("settings.savePerson", ExecutionPolicy::Worker, [](const Json &params) {
      Json saved = node_bridge::save_person(params);
      organisation::publish_own_profile();
      return saved;
    });
    register_handler("settings.saveLegalInstructions", ExecutionPolicy::Worker,
                     [](const Json &params) {
                       return node_bridge::save_legal_instruction_settings(params);
                     });

    // --- Notification Center / banners (local dismissal only) ---------

    register_handler("notices.getDismissedIds", ExecutionPolicy::Worker,
                     node_bridge::get_dismissed_notice_ids);

    register_handler("notices.dismiss", ExecutionPolicy::Worker,
                     node_bridge::dismiss_notice);

    // --- Workspaces ---------------------------------------------------

    register_handler("workspace.list", ExecutionPolicy::Worker, node_bridge::list_workspaces);

    // Opens the native folder picker (UI thread) and, unless cancelled,
    // adds the chosen folder as a workspace (or re-activates it if it
    // already is one).
    register_handler("workspace.choose", ExecutionPolicy::UiThread,
                     [this](const Json &) -> Json {
                       return node_bridge::choose_workspace(dialogs_.choose_folder());
                     });

    register_handler("workspace.remove", ExecutionPolicy::Worker,
                     node_bridge::remove_workspace);

    register_handler("workspace.setActive", ExecutionPolicy::Worker,
                     node_bridge::set_active_workspace);

    // Re-points an existing workspace entry at a newly chosen folder,
    // keeping its id (and hence its identity in the UI) - the fix for
    // "this workspace's folder is no longer available".
    register_handler("workspace.relocate", ExecutionPolicy::UiThread,
                     [this](const Json &params) -> Json {
                       node_bridge::validate_relocation(params);
                       // Never hold the persistence lock while AppKit runs a
                       // modal folder picker.
                       return node_bridge::relocate_workspace(params, dialogs_.choose_folder());
                     });

    // --- Model providers -----------------------------------------------

    register_handler("providers.list", ExecutionPolicy::Worker, provider_bridge::list);
    register_handler("providers.configure", ExecutionPolicy::Worker, provider_bridge::configure);
    register_handler("providers.disconnect", ExecutionPolicy::Worker, provider_bridge::disconnect);

    // --- Protected API ---------------------------------------------------

    register_handler("protectedApi.getPolicy", ExecutionPolicy::Worker,
                     papinian_guard_bridge::get_policy);

    register_handler("protectedApi.savePolicy", ExecutionPolicy::Worker,
                     papinian_guard_bridge::save_policy);

    // Runs the caller's text through routing::production_guard() - the exact
    // same Papinian Guard singleton every real Matter task/model.execute call
    // uses - or a worked example if none was given, never a hardcoded
    // "before/after" pair.
    register_handler("protectedApi.preview", ExecutionPolicy::Worker,
                     papinian_guard_bridge::preview);

    // What Papinian Guard hides in one cited passage, for the source view.
    register_handler("protectedApi.previewPassage", ExecutionPolicy::Worker,
                     papinian_guard_bridge::preview_passage);
    // A whole document as a model receives it; a long one on the pool of
    // runs, so the window's reads never wait behind it.
    register_handler("protectedApi.previewDocument", ExecutionPolicy::Worker,
                     papinian_guard_bridge::preview_document);

    // Reports Papinian's on-device model load status. Warm-up is started
    // once at app launch (see Impl's constructor above); the frontend polls
    // this until status leaves "loading" - see contract.ts's
    // PapinianModelStatus.
    register_handler("papinian.getModelStatus", ExecutionPolicy::Worker,
                     local_model_bridge::papinian_status);

    register_handler("localModels.authorize", ExecutionPolicy::Worker,
                     [this](const Json &params) {
                       const platform::HttpCancelScope downloads(package_downloads_);
                       return local_model_bridge::authorize(params);
                     });

    register_handler("localModels.sync", ExecutionPolicy::Worker,
                     [this](const Json &params) {
                       const platform::HttpCancelScope downloads(package_downloads_);
                       return local_model_bridge::sync(params);
                     });

    // --- Legal Machines account ----------------------------------------

    // Signing in on this computer: the password goes to Legal Machines over
    // HTTPS for this call only; the account kept is identifiers and names; the
    // download session the password is exchanged for is kept native-only
    // (ModelPackageManager). Once onboarding is complete with protection on, a
    // missing protection model's download starts then on its own worker; in
    // onboarding the member starts it. papinian.getModelStatus follows it.
    register_handler("account.signIn", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::sign_in(params, account_services()); });
    register_handler("account.verifyTwoFactor", ExecutionPolicy::Worker,
                     [this](const Json &params) {
                       return account_bridge::verify_two_factor(params, account_services());
                     });
    register_handler("account.lookup", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::lookup(params, account_services()); });
    register_handler("account.signInWithEmail", ExecutionPolicy::Worker,
                     [this](const Json &params) {
                       return account_bridge::sign_in_with_email(params, account_services());
                     });
    // A seat's password in this computer's user's own store: never
    // answered with, never logged.
    register_handler("account.seatPasswordStore", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::seat_password_store(params, account_services()); });
    register_handler("account.savedSeatPassword", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::saved_seat_password(params, account_services()); });
    register_handler("account.saveSeatPassword", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::save_seat_password(params, account_services()); });
    register_handler("account.forgetSeatPassword", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::forget_seat_password(params, account_services()); });
    register_handler("account.register", ExecutionPolicy::Worker,
                     [this](const Json &params) {
                       return account_bridge::register_organisation(params, account_services());
                     });
    // A new organisation, one thing a page: created from its details and
    // agreements, then its setup - the password, the authenticator app, its
    // recovery codes - opened by what its confirmed email gives, which stays
    // native.
    register_handler("account.create", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::create_organisation(params, account_services()); });
    register_handler("account.setPassword", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::set_password(params, account_services()); });
    register_handler("account.startTwoFactor", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::start_two_factor(params, account_services()); });
    register_handler("account.confirmTwoFactor", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::confirm_two_factor(params, account_services()); });
    // The Save dialog opens on the UI thread.
    register_handler("account.saveRecoveryCodes", ExecutionPolicy::UiThread,
                     [this](const Json &params) { return account_bridge::save_recovery_codes(params, account_services()); });
    register_handler("account.printRecoveryCodes", ExecutionPolicy::UiThread,
                     [this](const Json &params) { return account_bridge::print_recovery_codes(params, account_services()); });
    register_handler("account.confirmEmail", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::confirm_email(params, account_services()); });
    register_handler("account.resendEmailCode", ExecutionPolicy::Worker,
                     [this](const Json &params) {
                       return account_bridge::resend_email_code(params, account_services());
                     });
    register_handler("account.approvalState", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::approval_state(params, account_services()); });
    register_handler("account.listSeats", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::list_seats(params, account_services()); });
    register_handler("account.createSeat", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::create_seat(params, account_services()); });
    register_handler("account.setSeatPassword", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::set_seat_password(params, account_services()); });
    register_handler("account.removeSeat", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::remove_seat(params, account_services()); });
    register_handler("account.resetSeatTwoFactor", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::reset_seat_two_factor(params, account_services()); });
    register_handler("account.ownSeat", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::own_seat(params, account_services()); });
    register_handler("account.startOwnTwoFactor", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::start_own_two_factor(params, account_services()); });
    register_handler("account.confirmOwnTwoFactor", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::confirm_own_two_factor(params, account_services()); });
    register_handler("account.replaceOwnTwoFactor", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::replace_own_two_factor(params, account_services()); });
    register_handler("account.confirmReplacedTwoFactor", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::confirm_replaced_two_factor(params, account_services()); });
    register_handler("account.renewOwnRecoveryCodes", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::renew_own_recovery_codes(params, account_services()); });
    register_handler("account.changeOwnPassword", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::change_own_password(params, account_services()); });
    register_handler("account.requestRecovery", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::request_recovery(params, account_services()); });
    register_handler("account.resetPassword", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::reset_password(params, account_services()); });
    register_handler("account.reportProblem", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::report_problem(params, account_services()); });
    // A design partner's work log of one chat, sealed to Legal Machines.
    register_handler("account.workLogPreview", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::work_log_preview(params, account_services()); });
    register_handler("account.shareWorkLog", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::share_work_log(params, account_services()); });
    register_handler("account.getState", ExecutionPolicy::Worker,
                     [this](const Json &params) { return account_bridge::get_state(params, account_services()); });
    // Signed out, the window opens at the entry again, in onboarding's fixed
    // size, as at a first launch.
    register_handler("account.signOut", ExecutionPolicy::Worker, [this](const Json &params) {
      Json state = account_bridge::sign_out(params, account_services());
      window_.dispatch([this] {
        constexpr auto size = app::kOnboardingWindowSize;
        window_.clear_size_constraints();
        window_.set_size(size.width, size.height);
        window_.set_minimum_size(size.width, size.height);
        window_.set_maximum_size(size.width, size.height);
      });
      return state;
    });

    // The general local-AI-modules catalogue behind a future Settings
    // "Local Models" section - see LocalModel/LocalModelRegistry/LocalModelRegistry.hpp. One
    // real entry (Papinian NER) today; the registry itself already supports
    // more, so a future module is a new registration, not a UI redesign.
    register_handler("localModels.list", ExecutionPolicy::Worker, local_model_bridge::list);
    register_handler("localModels.getRuntimeState", ExecutionPolicy::Worker,
                     local_model_bridge::runtime_state);
    register_handler("localModels.load", ExecutionPolicy::Worker, local_model_bridge::load);
    register_handler("localModels.unload", ExecutionPolicy::Worker, local_model_bridge::unload);

		// Local storage/Manage's "Remove from this computer" - deletes an
		// optional module's installed package from disk (Phase 07). Distinct
		// from unload above; never available for a required module - see
		// local_model_bridge::remove().
		register_handler("localModels.remove", ExecutionPolicy::Worker, local_model_bridge::remove);

		// Choosing needs the file picker, so it runs on the UI thread like every
		// other dialog.
		register_handler("localModels.useExistingFile", ExecutionPolicy::UiThread,
		                 [this](const Json &) {
		                   return local_model_bridge::use_existing_file(
		                       dialogs_.choose_file("Model file", "gguf"));
		                 });
		register_handler("localModels.forgetExistingFile", ExecutionPolicy::Worker,
		                 local_model_bridge::forget_existing_file);
    register_handler("node.getSummary", ExecutionPolicy::Worker, node_bridge::get_summary);
    register_handler("node.checkLegalMachinesConnection", ExecutionPolicy::Worker,
                     node_bridge::check_legal_machines_connection);

    // The emergency "severe-state memory release pass" - stops non-required
    // local models to reclaim memory, never Project/.lm state. See
    // LocalModel/LocalModelRegistry/LocalModelRegistry.hpp's release_reclaimable_memory().
    register_handler("node.releaseReclaimableMemory", ExecutionPolicy::Worker,
                     node_bridge::release_reclaimable_memory);

    // --- Routing / inference --------------------------------------------

    // The one place a workspace, a default (or explicit) model, a connected provider's
    // secret and the Protected API policy come together to actually run a prompt - see
    // Routing/InferenceExecutor/InferenceExecutor.*. It sent a real, billed provider request
    // and returned its text without creating any canonical operation - money spent against a
    // person's Project with no record that it happened, no provenance and nothing in their
    // Usage. A product whose cost must be auditable cannot expose that, and nothing needs
    // it: every real request goes through chat.send, which records one canonical operation
    // with its own usage, provenance and protection.

    // Phase 06 - role/capability metadata and the effective-setup
    // resolver. Pure reads: neither mutates Project/.lm or Node settings
    // state.
    register_handler("routing.getAiSetupCapabilities", ExecutionPolicy::Worker,
                     routing_bridge::get_ai_setup_capabilities);
    register_handler("routing.getEffectiveAiSetup", ExecutionPolicy::Worker,
                     routing_bridge::get_effective_ai_setup);

    // --- Matters ----------------------------------------------------------

    register_handler("matter.list", ExecutionPolicy::Worker, matter_bridge::list);
    register_handler("matter.listSummaries", ExecutionPolicy::Worker, with_viewer(matter_bridge::list_summaries));
    register_handler("routingLog.list", ExecutionPolicy::Worker, usage_bridge::list);
    register_handler("usage.getSummary", ExecutionPolicy::Worker, usage_bridge::get_summary);
    register_handler("usage.generateProjectStatement", ExecutionPolicy::UiThread,
                     [this](const Json& params) { return generate_project_usage_statement(params); });
    register_handler("usage.saveWorkStatement", ExecutionPolicy::UiThread,
                     [this](const Json& params) { return save_work_statement(params); });
    register_handler("usage.confirmCosts", ExecutionPolicy::Worker, usage_bridge::confirm_costs);
    // This computer's work record, kept from now on and timestamped as it grows.
    usage_bridge::start_work_record();

    register_handler("matter.create", ExecutionPolicy::Worker, matter_bridge::create);

    register_handler("matter.update", ExecutionPolicy::Worker, matter_bridge::update);

    register_handler("matter.delete", ExecutionPolicy::Worker, matter_bridge::remove);
    // This computer's Trash of Matters.
    register_handler("project.listTrash", ExecutionPolicy::Worker, matter_bridge::list_trash);
    register_handler("project.moveToTrash", ExecutionPolicy::Worker, matter_bridge::move_to_trash);
    register_handler("project.restoreFromTrash", ExecutionPolicy::Worker, matter_bridge::restore_from_trash);
    register_handler("project.deleteFromTrash", ExecutionPolicy::Worker, matter_bridge::delete_from_trash);

    // Opens the native multi-file picker (UI thread) and, unless cancelled,
    // copies every chosen file into the Matter's folder, beside its `.lm`; a
    // Matter with no folder keeps a copy in Singularity's own store
    // (Document/DocumentActions import_files).
    register_handler("matter.addDocument", ExecutionPolicy::UiThread,
                     [this](const Json &params) { return add_matter_document(params); });

    register_handler("matter.removeDocument", ExecutionPolicy::Worker, document_bridge::remove);

    register_handler("matter.revealDocument", ExecutionPolicy::UiThread, document_bridge::reveal);

    register_handler("matter.openDocument", ExecutionPolicy::UiThread, document_bridge::open);

		// Phase 08 (Documents - filesystem, list and states): reconciles the
		// Matter's real Project folder into matter.documents and returns the
		// full Matter, same shape as every other matter.* mutation above - see
		// Bridge/DocumentBridge/DocumentBridge.cpp.
		register_handler("matter.listDocuments", ExecutionPolicy::Worker, document_bridge::list_documents);

    // Phase 09 real filesystem actions for a Referenced document - each
    // mutates the real Project folder first, then returns the freshly
    // reconciled Matter (matter.deleteDocument throws instead of resolving
    // when the file could not genuinely reach the OS Trash) - see
    // Bridge/DocumentBridge/DocumentBridge.cpp.
    register_handler("matter.renameDocument", ExecutionPolicy::Worker, document_bridge::rename_document);
    register_handler("matter.moveDocument", ExecutionPolicy::Worker, document_bridge::move_document);
    register_handler("matter.deleteDocument", ExecutionPolicy::Worker, document_bridge::delete_document);
    register_handler("matter.createFolder", ExecutionPolicy::Worker, document_bridge::create_folder);
    register_handler("matter.addExternalFile", ExecutionPolicy::Worker, document_bridge::add_external_file);
    register_handler("document.addExternalFolder", ExecutionPolicy::Worker, document_bridge::add_external_folder);
    register_handler("matter.reprocessDocument", ExecutionPolicy::Worker, document_bridge::reprocess_document);
    register_handler("matter.resolveDocumentSource", ExecutionPolicy::Worker, document_bridge::resolve_document_source);
    register_handler("matter.readDocumentView", ExecutionPolicy::Worker, document_bridge::read_document_view);
    register_handler("document.listVersions", ExecutionPolicy::Worker, document_bridge::list_versions);
    register_handler("document.restoreVersion", ExecutionPolicy::Worker, document_bridge::restore_version);

    register_handler("chat.list", ExecutionPolicy::Worker, chat_bridge::list);
    register_handler("chat.create", ExecutionPolicy::Worker, chat_bridge::create_project_chat);
    register_handler("chat.rename", ExecutionPolicy::Worker, chat_bridge::rename);
    register_handler("chat.setArchived", ExecutionPolicy::Worker, chat_bridge::set_archived);
    // Phase 27's restart-surviving draft, real backend deletion-dependency
    // check and Chat deletion - see ChatBridge.hpp's own doc comments.
    register_handler("chat.saveDraft", ExecutionPolicy::Worker, chat_bridge::save_draft);
    register_handler("chat.post", ExecutionPolicy::Worker, agent_bridge::post_message);
    register_handler("chat.editMessage", ExecutionPolicy::Worker, chat_bridge::edit_message);
    register_handler("chat.deleteMessage", ExecutionPolicy::Worker, chat_bridge::delete_message);
    register_handler("chat.react", ExecutionPolicy::Worker, chat_bridge::react);
    register_handler("chat.markReceived", ExecutionPolicy::Worker, chat_bridge::mark_received);
    register_handler("chat.readMarks", ExecutionPolicy::Worker, chat_bridge::read_marks);
    register_handler("chat.markRead", ExecutionPolicy::Worker, chat_bridge::mark_read);
    register_handler("chat.closeThread", ExecutionPolicy::Worker, chat_bridge::close_thread);
    register_handler("chat.reopenThread", ExecutionPolicy::Worker, chat_bridge::reopen_thread);
    // A colleague's document review in a thread: its versions, the lawyer's
    // decision on one, a retry of one that failed (DocumentReview/). A Playbook
    // colleague asked in a thread for a review carries it out.
    document_review::installForColleagues();
    register_handler("documentReview.list", ExecutionPolicy::Worker, document_review_bridge::list);
    register_handler("documentReview.get", ExecutionPolicy::Worker, document_review_bridge::get);
    register_handler("documentReview.decide", ExecutionPolicy::Worker, document_review_bridge::decide);
    register_handler("documentReview.retry", ExecutionPolicy::Worker, document_review_bridge::retry);
    // The assignments, step by step with their email's readiness, and a version
    // made again without the changes the lawyer leaves out.
    register_handler("documentReview.assignments", ExecutionPolicy::Worker, document_review_bridge::assignments);
    register_handler("documentReview.leaveOut", ExecutionPolicy::Worker, document_review_bridge::leaveOut);
    register_handler("documentReview.answerDecision", ExecutionPolicy::Worker, document_review_bridge::answerDecision);
    // A document a colleague wrote, accepted or returned as a revised draft is.
    register_handler("documentReview.decideWritten", ExecutionPolicy::Worker, document_review_bridge::decideWritten);
    // A copy of a version's revised draft, where the lawyer chooses: the file
    // is found before the save dialog opens on the window's thread.
    register_handler("documentReview.saveCopy", ExecutionPolicy::UiThread, [this](const Json &params) {
      const document_review_bridge::PreparedCopy prepared = document_review_bridge::prepareCopy(params);
      const std::optional<std::string> chosen = dialogs_.choose_save_path(prepared.defaultName, "Word document", "docx");
      std::optional<std::filesystem::path> destination;
      if (chosen.has_value() == true) destination = std::filesystem::u8path(*chosen);
      return document_review_bridge::finishCopy(prepared, destination);
    });
    register_handler("chat.setWorkingDocument", ExecutionPolicy::Worker, chat_bridge::set_working_document);
    register_handler("chat.deletionImpact", ExecutionPolicy::Worker, chat_bridge::deletion_impact);
    register_handler("chat.delete", ExecutionPolicy::Worker, chat_bridge::delete_chat);
    register_handler("chat.restore", ExecutionPolicy::Worker, chat_bridge::restore);
    // Reads and rewrites durable Chat state only; no provider, no model.
    register_handler("chat.compact", ExecutionPolicy::Worker, chat_bridge::compact);
    // Phase 27's Search - a real backend text match over Chat title/
    // prompts/response text, never a frontend-only filter.
    register_handler("chat.search", ExecutionPolicy::Worker, chat_bridge::search);
    register_handler("matter.createChat", ExecutionPolicy::Worker, chat_bridge::create);
    register_handler("matter.saveChatAiSetup", ExecutionPolicy::Worker, chat_bridge::save_ai_setup);
    register_handler("chat.resolveSources", ExecutionPolicy::Worker, chat_bridge::resolve_sources);
    register_handler("chat.legalTaskOptions", ExecutionPolicy::Worker, chat_bridge::legal_task_options);
    register_handler("chat.resolveOperationSetup", ExecutionPolicy::Worker, chat_bridge::resolve_operation_setup);

    // Phase 23 (Chats - Papinian state and outbound preview). Neither
    // persists anything, creates a MatterTask or makes a provider/network
    // call; the preview reuses the existing Phase 11 Papinian preview and
    // ephemeral preview-session machinery through the canonical request
    // preparation path - see chat::preview_chat_outbound_request().
    register_handler("chat.resolveProtectionState", ExecutionPolicy::Worker,
                     chat_preview_bridge::resolve_protection_state);
    register_handler("chat.previewOutboundRequest", ExecutionPolicy::Worker,
                     chat_preview_bridge::preview_outbound_request);
    register_handler("chat.send", ExecutionPolicy::Worker, chat_execution_bridge::send);
	register_handler("chat.resumeOperation", ExecutionPolicy::Worker,
		chat_execution_bridge::resume_operation);
    // Stop and its lifecycle read update the live coordination record at once
    // and mirror lifecycle truth into the canonical MatterTask - never a
    // provider or model. They run on the control pool (is_operation_control()):
    // deliverable while the runs they stop hold their own pool, and never on the
    // window's thread, where waiting for a save would freeze the window.
    register_handler("chat.stopOperation", ExecutionPolicy::Worker,
                     chat_execution_bridge::stop_operation);
    register_handler("chat.operationState", ExecutionPolicy::Worker,
                     chat_execution_bridge::operation_state);
	// Reads and reconciles durable records only; no provider, no model.
	register_handler("chat.listOperations", ExecutionPolicy::Worker,
		chat_execution_bridge::list_operations);
    register_handler("chat.actionPermission", ExecutionPolicy::Worker,
                     chat_execution_bridge::action_permission);
    register_handler("chat.saveActionPermission", ExecutionPolicy::Worker,
                     chat_execution_bridge::save_action_permission);
    // This only records a trusted decision and wakes the original operation thread.
    // The same for the lawyer's answer to a question: the run that asked it holds a
    // run thread while it waits, and the answer comes through the control pool, so
    // it never queues behind the very run it wakes. A question or a web search left
    // waiting when Singularity closed is answered on the place its run kept; the run
    // then goes on from there, on the run pool like every run
    // (chat::paused_decision_awaits_resume). So does a change to a Project document
    // approved once its run had returned: the decision carries it on, and no client
    // asks for a resume.
    register_handler("chat.respondToolApproval", ExecutionPolicy::Worker,
                     [this](const Json &params) {
                       Json status = chat_execution_bridge::respond_tool_approval(params);
                       resume_paused_decision(params);
                       return status;
                     });
    register_handler("chat.answerQuestion", ExecutionPolicy::Worker,
                     [this](const Json &params) {
                       Json status = chat_execution_bridge::answer_question(params);
                       resume_paused_decision(params);
                       return status;
                     });
    register_handler("chat.conversation", ExecutionPolicy::Worker, with_viewer(chat_execution_bridge::conversation));
    register_handler("chat.resolveSourceReference", ExecutionPolicy::Worker,
                     chat_execution_bridge::resolve_source_reference);
    register_handler("matter.chooseFolder", ExecutionPolicy::UiThread,
                     [this](const Json &params) {
                       return matter_bridge::assign_folder(params, dialogs_.choose_folder());
                     });
    // The folder is chosen in the native dialog on the window's thread; the
    // Project is set up from it on a worker, behind the Project lock.
    register_handler("matter.createFromFolder", ExecutionPolicy::UiThread,
                     [this](const Json &) -> Json {
                       const std::optional<std::string> folder = dialogs_.choose_folder();
                       return Json{{"folder", folder ? Json(*folder) : Json(nullptr)}};
                     },
                     [](const Json &chosen) -> Json {
                       const Json &folder = chosen.at("folder");
                       return matter_bridge::create_from_folder(
                           folder.is_string() ? std::optional<std::string>(folder.get<std::string>()) : std::nullopt);
                     });
    register_handler("matter.openFolder", ExecutionPolicy::UiThread, matter_bridge::open_folder);
    // A second lawyer joins the firm's shared Matter: one discovery found in the
    // folders they gave Singularity, opened without a dialog.
    register_handler("matter.openDiscovered", ExecutionPolicy::Worker, matter_bridge::open_discovered);
    register_handler("coordination.useFirmFolder", ExecutionPolicy::Worker, matter_bridge::coordination_use_firm_folder);
    register_handler("coordination.status", ExecutionPolicy::Worker, matter_bridge::coordination_status);
    register_handler("coordination.shareMatter", ExecutionPolicy::Worker, matter_bridge::coordination_share);
    register_handler("coordination.invitation", ExecutionPolicy::Worker, matter_bridge::coordination_invitation);
    register_handler("coordination.join", ExecutionPolicy::Worker, matter_bridge::coordination_join);
    register_handler("coordination.acceptKey", ExecutionPolicy::Worker, matter_bridge::coordination_accept_key);
    register_handler("coordination.firmComputers", ExecutionPolicy::Worker, matter_bridge::coordination_firm_computers);
    register_handler("coordination.checkComputer", ExecutionPolicy::Worker, matter_bridge::coordination_check_computer);
    register_handler("coordination.computerCheck", ExecutionPolicy::Worker, matter_bridge::coordination_computer_check);
    register_handler("coordination.keepComputer", ExecutionPolicy::Worker, matter_bridge::coordination_keep_computer);
    register_handler("coordination.refuseComputer", ExecutionPolicy::Worker, matter_bridge::coordination_refuse_computer);
    register_handler("coordination.endComputerCheck", ExecutionPolicy::Worker, matter_bridge::coordination_end_computer_check);
    register_handler("coordination.giveMatter", ExecutionPolicy::Worker, matter_bridge::coordination_give_matter);
    register_handler("coordination.offers", ExecutionPolicy::Worker, matter_bridge::coordination_offers);
    register_handler("coordination.acceptOffer", ExecutionPolicy::Worker, matter_bridge::coordination_accept_offer);
    register_handler("coordination.declineOffer", ExecutionPolicy::Worker, matter_bridge::coordination_decline_offer);
    register_handler("chat.openPersonal", ExecutionPolicy::Worker, matter_bridge::open_personal_chat);
    register_handler("chat.listPersonal", ExecutionPolicy::Worker, matter_bridge::list_personal_chats);
    // A personal conversation cleared by the member here (Chat/ConversationClearing).
    register_handler("chat.clearConversation", ExecutionPolicy::Worker, [](const Json &params) -> Json {
      const std::string chat_id = params.at("chatId").get<std::string>();
      const std::string now = workspaces::current_timestamp();
      const auto before = chat::conversation_clearing::clear(chat_id, now);
      return Json{{"chatId", chat_id}, {"clearedBefore", now}, {"previouslyClearedBefore", before ? Json(*before) : Json(nullptr)}};
    });
    register_handler("chat.restoreConversation", ExecutionPolicy::Worker, [](const Json &params) -> Json {
      const std::string chat_id = params.at("chatId").get<std::string>();
      const auto value = params.find("clearedBefore");
      const std::optional<std::string> cleared = value != params.end() && value->is_string() ? std::optional<std::string>(value->get<std::string>()) : std::nullopt;
      chat::conversation_clearing::restore(chat_id, cleared);
      return Json{{"chatId", chat_id}, {"clearedBefore", cleared ? Json(*cleared) : Json(nullptr)}};
    });
    register_handler("chat.clearedConversations", ExecutionPolicy::Worker, [](const Json &) -> Json {
      Json conversations = Json::array();
      for (const auto &[chat_id, at] : chat::conversation_clearing::all()) conversations.push_back(Json{{"chatId", chat_id}, {"clearedBefore", at}});
      return Json{{"conversations", std::move(conversations)}};
    });
    // The icons of the sites a message's web sources come from: each asked
    // of its own site, over https, with nothing of the firm's in the
    // request; every site at once and each ask a few seconds at most, so one
    // call holds its worker briefly. A member's Stop in a conversation:
    // every colleague on this computer ends the work it was given there and
    // stays at work for what it is given next (Agent/AgentActions
    // stop_work_in_chat).
    register_handler("chat.stopColleagues", ExecutionPolicy::Worker, [](const Json &params) -> Json {
      const std::string chat_id = params.at("chatId").get<std::string>();
      if (chat_id.empty()) throw std::invalid_argument("chatId must name a conversation.");
      return Json{{"chatId", chat_id}, {"ended", agents::stop_work_in_chat(chat_id)}};
    });
    register_handler("chat.sourceIcons", ExecutionPolicy::Worker, [](const Json &params) -> Json {
      const Json &sites = params.at("sites");
      if (!sites.is_array()) throw std::invalid_argument("sites must be a list of site names.");
      std::vector<std::string> names;
      for (const auto &site : sites) {
        if (site.is_string()) names.push_back(site.get<std::string>());
      }
      const auto icons = research::site_icon::fetch_all(names, [](const std::string &url) -> std::optional<std::string> {
        try {
          const auto response = platform::https_get(url, {{"Accept", "image/*,text/html;q=0.9,*/*;q=0.5"}}, 4);
          if (response.status >= 200 && response.status < 300) return response.body;
        } catch (const std::exception &) {
        }
        return std::nullopt;
      });
      Json found = Json::object();
      for (const auto &[site, icon] : icons) found[site] = icon ? Json(research::site_icon::data_url(*icon)) : Json(nullptr);
      return Json{{"icons", std::move(found)}};
    });
    // The sample matter: copied once into the default Projects folder and
    // opened, so a lawyer can try Singularity without a client's files.
    register_handler("project.createSample", ExecutionPolicy::Worker, matter_bridge::create_sample);
    // A new Matter in the folder the lawyer chose for it: the folder is the one
    // the system's dialog gave (matter.chooseNewFolder), named to the window by
    // a token used once, never a path the window wrote.
    register_handler("project.create", ExecutionPolicy::Worker, [this](const Json &params) -> Json {
      std::optional<std::filesystem::path> chosen;
      const auto token = params.find("folderToken");
      if (token != params.end() && token->is_string() == true && token->get<std::string>().empty() == false) {
        const std::lock_guard<std::mutex> lock(chosen_folders_mutex_);
        const auto found = chosen_folders_.find(token->get<std::string>());
        if (found == chosen_folders_.end()) throw std::invalid_argument("Choose the Matter's folder again.");
        chosen = found->second;
        chosen_folders_.erase(found);
      }
      return matter_bridge::create_named_in(params, chosen);
    });
    register_handler("matter.chooseNewFolder", ExecutionPolicy::UiThread, [this](const Json &) -> Json {
      const std::optional<std::string> folder = dialogs_.choose_folder();
      if (!folder) return Json(nullptr);
      const std::filesystem::path path = std::filesystem::u8path(*folder);
      const std::string token = platform::secure_random_hex(16);
      {
        const std::lock_guard<std::mutex> lock(chosen_folders_mutex_);
        chosen_folders_[token] = path;
      }
      std::error_code error;
      Json chosen{{"token", token}, {"path", *folder}, {"name", path.filename().u8string()},
          {"holdsMatter", std::filesystem::exists(path / ".lm", error)},
          // The Matter it holds, and where it stands here.
          {"heldMatter", matter_bridge::held_matter(path)}};
      return chosen;
    });

    // Master Spec Sec.27 (Matter Notes) - a lawyer's own durable copy of a
    // useful AI answer. No dialog, no provider call - pure local persistence.
    register_handler("matter.addNote", ExecutionPolicy::Worker, matter_bridge::add_note);

    register_handler("matter.removeNote", ExecutionPolicy::Worker, matter_bridge::remove_note);

    // Master Spec Sec.26 (Issues) - promotion from an already-persisted
    // Finding. No dialog, no provider call - pure local persistence.
    register_handler("matter.addIssue", ExecutionPolicy::Worker, matter_bridge::add_issue);

    register_handler("matter.updateIssueStatus", ExecutionPolicy::Worker,
                     matter_bridge::update_issue_status);

		register_handler("workProduct.listTypes", ExecutionPolicy::Worker, work_product_bridge::list_types);
		register_handler("workProduct.createType", ExecutionPolicy::Worker, work_product_bridge::create_type);
		register_handler("workProduct.list", ExecutionPolicy::Worker, work_product_bridge::list_objects);
		register_handler("workProduct.get", ExecutionPolicy::Worker, work_product_bridge::get_detail);
		register_handler("workProduct.create", ExecutionPolicy::Worker, work_product_bridge::create);
		register_handler("workProduct.retainTaskResult", ExecutionPolicy::Worker,
				work_product_bridge::retain_task_result);
		register_handler("workProduct.update", ExecutionPolicy::Worker, work_product_bridge::update);
		register_handler("workProduct.updateIssueStatus", ExecutionPolicy::Worker,
				work_product_bridge::update_issue_status);
		register_handler("workProduct.restoreRevision", ExecutionPolicy::Worker,
				work_product_bridge::restore_revision);
		register_handler("workProduct.remove", ExecutionPolicy::Worker, work_product_bridge::remove);
		register_handler("reviewTable.list", ExecutionPolicy::Worker, review_table_bridge::list);
		register_handler("reviewTable.get", ExecutionPolicy::Worker, review_table_bridge::get);
		register_handler("reviewTable.create", ExecutionPolicy::Worker, review_table_bridge::create);
		register_handler("reviewTable.update", ExecutionPolicy::Worker, review_table_bridge::update);
		register_handler("reviewTable.remove", ExecutionPolicy::Worker, review_table_bridge::remove);
		register_handler("reviewTable.run", ExecutionPolicy::Worker, review_table_bridge::run);
		register_handler("reviewTable.stop", ExecutionPolicy::Worker, review_table_bridge::stop);
		register_handler("reviewTable.export", ExecutionPolicy::Worker, review_table_bridge::export_table);
		register_handler("reviewTable.prepareChanges", ExecutionPolicy::Worker, playbook_bridge::prepare_changes);
		register_handler("reviewTable.stopChanges", ExecutionPolicy::Worker, playbook_bridge::stop_changes);
		register_handler("playbook.list", ExecutionPolicy::Worker, playbook_bridge::list);
		register_handler("playbook.get", ExecutionPolicy::Worker, playbook_bridge::get);
		register_handler("playbook.save", ExecutionPolicy::Worker, playbook_bridge::save);
		register_handler("playbook.remove", ExecutionPolicy::Worker, playbook_bridge::remove);
		register_handler("playbook.import", ExecutionPolicy::Worker, playbook_bridge::import_playbook);
		register_handler("playbook.export", ExecutionPolicy::Worker, playbook_bridge::export_playbook);
		register_handler("playbook.reviewTable", ExecutionPolicy::Worker, playbook_bridge::review_table);
		register_handler("agent.list", ExecutionPolicy::Worker, agent_bridge::list);
		register_handler("agent.get", ExecutionPolicy::Worker, with_viewer(agent_bridge::get));
		register_handler("agent.roles", ExecutionPolicy::Worker, agent_bridge::roles);
		// The organisation: its members, areas, work and decisions.
		register_handler("organisation.get", ExecutionPolicy::Worker, organisation_bridge::get);
		register_handler("organisation.setOwnOffice", ExecutionPolicy::Worker, organisation_bridge::set_own_office);
		register_handler("organisation.setOwnPhoto", ExecutionPolicy::Worker, organisation_bridge::set_own_photo);
		// The system's own dialog, for one picture; the window cuts the photo.
		register_handler("organisation.chooseOwnPhoto", ExecutionPolicy::UiThread, [this](const Json &) -> Json {
			return organisation_bridge::own_photo_picture(dialogs_.choose_file("Picture", "jpg,jpeg,png,gif,webp"));
		});
		// Singularity in Word, Excel and Outlook.
		register_handler("office.getStatus", ExecutionPolicy::Worker, office_bridge::get_status);
		register_handler("office.connect", ExecutionPolicy::Worker, office_bridge::connect);
		register_handler("office.openApp", ExecutionPolicy::Worker, office_bridge::open_app);
		register_handler("office.disconnect", ExecutionPolicy::Worker, office_bridge::disconnect);
		register_handler("office.listClients", ExecutionPolicy::Worker, office_bridge::list_clients);
		register_handler("office.decidePairing", ExecutionPolicy::Worker, office_bridge::decide_pairing);
		register_handler("office.removeClient", ExecutionPolicy::Worker, office_bridge::remove_client);
		register_handler("office.describeFile", ExecutionPolicy::Worker, office_bridge::describe_file);
		register_handler("office.fileIntoMatter", ExecutionPolicy::Worker, office_bridge::file_into_matter);
		register_handler("office.documentContent", ExecutionPolicy::Worker, office_bridge::document_content);
		register_handler("office.emailDraft", ExecutionPolicy::Worker, office_bridge::email_draft);
		register_handler("office.openDocument", ExecutionPolicy::Worker, office_bridge::open_document);
		register_handler("office.reveal", ExecutionPolicy::Worker, office_bridge::reveal);
		register_handler("correspondence.suggest", ExecutionPolicy::Worker, correspondence_bridge::suggest);
		register_handler("correspondence.file", ExecutionPolicy::Worker, correspondence_bridge::file);
		register_handler("correspondence.move", ExecutionPolicy::Worker, correspondence_bridge::move);
		register_handler("correspondence.unfile", ExecutionPolicy::Worker, correspondence_bridge::unfile);
		register_handler("correspondence.list", ExecutionPolicy::Worker, correspondence_bridge::list);
		register_handler("correspondence.draftReply", ExecutionPolicy::UiThread, correspondence_bridge::draft_reply);
		register_handler("correspondence.saveAttachment", ExecutionPolicy::Worker, correspondence_bridge::save_attachment);
		register_handler("project.recordTime", ExecutionPolicy::Worker, time_bridge::record);
		register_handler("project.updateTime", ExecutionPolicy::Worker, time_bridge::update);
		register_handler("project.removeTime", ExecutionPolicy::Worker, time_bridge::remove);
		register_handler("project.listTime", ExecutionPolicy::Worker, time_bridge::list);
		register_handler("agent.getRun", ExecutionPolicy::Worker, agent_bridge::get_run);
		register_handler("agent.create", ExecutionPolicy::Worker, agent_bridge::create);
		register_handler("agent.hire", ExecutionPolicy::Worker, agent_bridge::hire);
		register_handler("agent.memory", ExecutionPolicy::Worker, agent_bridge::memory);
		register_handler("agent.firmColleagues", ExecutionPolicy::Worker, agent_bridge::firm_colleagues);
		register_handler("agent.forgetMemory", ExecutionPolicy::Worker, agent_bridge::forget_memory);
		register_handler("agent.approve", ExecutionPolicy::Worker, agent_bridge::approve);
		register_handler("agent.decline", ExecutionPolicy::Worker, agent_bridge::decline);
		register_handler("agent.pause", ExecutionPolicy::Worker, agent_bridge::pause);
		register_handler("agent.setModel", ExecutionPolicy::Worker, agent_bridge::set_model);
		register_handler("agent.setMemoryModel", ExecutionPolicy::Worker, agent_bridge::set_memory_model);
		register_handler("agent.resume", ExecutionPolicy::Worker, agent_bridge::resume);
		register_handler("agent.stop", ExecutionPolicy::Worker, agent_bridge::stop);
		register_handler("agent.edit", ExecutionPolicy::Worker, agent_bridge::edit);
		register_handler("agent.requestEdit", ExecutionPolicy::Worker, agent_bridge::request_edit);
		register_handler("agent.archive", ExecutionPolicy::Worker, agent_bridge::archive);
		register_handler("agent.delete", ExecutionPolicy::Worker, agent_bridge::remove);
		register_handler("agent.update", ExecutionPolicy::Worker, agent_bridge::update);
		register_handler("agent.setOffice", ExecutionPolicy::Worker, agent_bridge::set_office);
		register_handler("agent.run", ExecutionPolicy::Worker, agent_bridge::run);
		register_handler("agent.cancelRun", ExecutionPolicy::Worker, agent_bridge::cancel_run);
		register_handler("agent.markReviewed", ExecutionPolicy::Worker, agent_bridge::mark_reviewed);
		register_handler("agent.assign", ExecutionPolicy::Worker, agent_bridge::assign);
		register_handler("agent.withdrawAssignment", ExecutionPolicy::Worker, agent_bridge::withdraw_assignment);
		register_handler("agent.previewSetup", ExecutionPolicy::Worker, agent_bridge::preview_setup);
		// The panel opens on the window's thread, in the Matter's folder the
		// page names; the folder is checked against the Matter on a worker.
		register_handler("agent.chooseFolder", ExecutionPolicy::UiThread,
				[this](const Json &params) -> Json {
					// The Matter the folder is for is named before the dialog
					// opens, so a call without one is refused before the
					// person picks anything.
					const std::string project_id = params.at("projectId").get<std::string>();
					std::optional<std::string> start;
					const auto given = params.find("startPath");
					if (given != params.end() && given->is_string() == true) start = given->get<std::string>();
					const std::optional<std::string> folder = dialogs_.choose_folder(start);
					return Json{{"folder", folder ? Json(*folder) : Json(nullptr)}, {"projectId", project_id}};
				},
				[](const Json &chosen) -> Json {
					const Json &folder = chosen.at("folder");
					return agent_bridge::chosen_folder(chosen,
							folder.is_string() ? std::optional<std::string>(folder.get<std::string>()) : std::nullopt);
				});
		register_handler("agent.openFile", ExecutionPolicy::UiThread, [](const Json &params) -> Json {
			return platform::open_in_default_application(agent_bridge::run_file_path(params));
		});
		register_handler("agent.revealFile", ExecutionPolicy::UiThread, [](const Json &params) -> Json {
			return platform::reveal_in_file_manager(agent_bridge::run_file_path(params));
		});
		register_handler("agent.listAiApps", ExecutionPolicy::Worker, agent_bridge::ai_apps);
		register_handler("agent.connectAiApp", ExecutionPolicy::Worker, agent_bridge::connect_app);
		register_handler("agent.renewAiAppKey", ExecutionPolicy::Worker, agent_bridge::renew_app_key);
		register_handler("agent.disconnectAiApp", ExecutionPolicy::Worker, agent_bridge::disconnect_app);

		register_handler("consilium.list", ExecutionPolicy::Worker, consilium_bridge::list);
		register_handler("consilium.create", ExecutionPolicy::Worker, consilium_bridge::create);
		register_handler("consilium.update", ExecutionPolicy::Worker, consilium_bridge::update);
		register_handler("consilium.duplicate", ExecutionPolicy::Worker, consilium_bridge::duplicate);
		register_handler("consilium.import", ExecutionPolicy::UiThread,
				[this](const Json& params) { return consilium_bridge::import_selected(params, dialogs_.choose_files()); });
		register_handler("consilium.export", ExecutionPolicy::UiThread,
				[this](const Json& params) {
					const std::string name = consilium_bridge::export_default_name(params);
					return consilium_bridge::export_selected(
						params, dialogs_.choose_save_path(name, "Consilium configuration", "json"));
				});

    // --- Privacy (Phase 10 - entities and Project coverage) ---------------
    //
    // Project coverage, documents needing attention and the Entity Registry
    // review surface - real local Project state (Bridge/PrivacyBridge/
    // PrivacyBridge.cpp), never the ephemeral request-scoped Restoration Map
    // Phase 11 owns.

    register_handler("privacy.getOverview", ExecutionPolicy::Worker, privacy_bridge::get_overview);
    register_handler("privacy.getCoverage", ExecutionPolicy::Worker, privacy_bridge::get_coverage);
    register_handler("privacy.listAttentionDocuments", ExecutionPolicy::Worker,
                     privacy_bridge::list_attention_documents);
    register_handler("privacy.listEntities", ExecutionPolicy::Worker, privacy_bridge::list_entities);
    register_handler("privacy.getEntityDetail", ExecutionPolicy::Worker, privacy_bridge::get_entity_detail);
    register_handler("privacy.resolveAmbiguity", ExecutionPolicy::Worker, privacy_bridge::resolve_ambiguity);
    register_handler("privacy.splitEntityAlias", ExecutionPolicy::Worker, privacy_bridge::split_entity_alias);
    register_handler("privacy.addEntityAlias", ExecutionPolicy::Worker, privacy_bridge::add_entity_alias);
    register_handler("privacy.removeEntityAlias", ExecutionPolicy::Worker, privacy_bridge::remove_entity_alias);
    register_handler("privacy.getPassageDisclosure", ExecutionPolicy::Worker, privacy_bridge::get_passage_disclosure);
    register_handler("privacy.getOperationDisclosure", ExecutionPolicy::Worker, privacy_bridge::get_operation_disclosure);
    register_handler("context.listItems", ExecutionPolicy::Worker, project_context_bridge::list_items);
    register_handler("context.listSuggestions", ExecutionPolicy::Worker, project_context_bridge::list_suggestions);
    register_handler("context.searchItems", ExecutionPolicy::Worker, project_context_bridge::search_items);
    register_handler("context.getPageState", ExecutionPolicy::Worker, project_context_bridge::get_page_state);
    register_handler("context.runAutomaticUpdate", ExecutionPolicy::Worker, project_context_bridge::run_automatic_update);
    register_handler("context.acceptSuggestion", ExecutionPolicy::Worker, project_context_bridge::accept_suggestion);
    register_handler("context.dismissSuggestion", ExecutionPolicy::Worker, project_context_bridge::dismiss_suggestion);
    register_handler("context.createItem", ExecutionPolicy::Worker, project_context_bridge::create_item);
    register_handler("context.memoryChat", ExecutionPolicy::Worker, project_context_bridge::memory_chat);
    register_handler("context.memoryStatus", ExecutionPolicy::Worker, project_context_bridge::memory_status);
    register_handler("context.memoryReadDocuments", ExecutionPolicy::Worker,
                     project_context_bridge::memory_read_documents);
    register_handler("context.createItemFromChatResult", ExecutionPolicy::Worker, project_context_bridge::create_item_from_chat_result);
    register_handler("context.updateItem", ExecutionPolicy::Worker, project_context_bridge::update_item);
    register_handler("context.deleteItem", ExecutionPolicy::Worker, project_context_bridge::delete_item);
    register_handler("context.linkItemEntity", ExecutionPolicy::Worker, project_context_bridge::link_item_entity);
    register_handler("context.unlinkItemEntity", ExecutionPolicy::Worker, project_context_bridge::unlink_item_entity);
	register_handler("context.updateItemControls", ExecutionPolicy::Worker, project_context_bridge::update_item_controls);
	register_handler("context.listMergeConflicts", ExecutionPolicy::Worker, project_context_bridge::list_merge_conflicts);
	register_handler("context.resolveMergeConflict", ExecutionPolicy::Worker, project_context_bridge::resolve_merge_conflict);

    // Project Data - .lm. Public wire names use project.*; the C++
    // implementation stays in the Matter domain/Bridge internally -
    // matter_bridge maps projectId to the same Matter id. setUpData and
    // removeData touch .lm on disk; status/reveal do not mutate durable state
    // beyond a best-effort Finder/Explorer reveal.
    register_handler("project.getDataStatus", ExecutionPolicy::Worker,
                     matter_bridge::get_project_data_status);
    register_handler("project.setUpData", ExecutionPolicy::Worker,
                     matter_bridge::set_up_project_data);
    register_handler("project.revealData", ExecutionPolicy::UiThread,
                     matter_bridge::reveal_project_data);
    register_handler("project.prepareCleanCopy", ExecutionPolicy::UiThread,
                     [this](const Json &params) {
                       return matter_bridge::prepare_project_data_clean_copy(params, dialogs_.choose_folder());
                     });
    register_handler("project.removeData", ExecutionPolicy::Worker,
                     matter_bridge::remove_project_data);
    register_handler("project.discover", ExecutionPolicy::Worker,
                     matter_bridge::discover_projects);
	// Reads and resolves durable merge conflicts; no provider, no model.
	register_handler("project.listConflicts", ExecutionPolicy::Worker,
		matter_bridge::list_project_conflicts);
	register_handler("project.resolveConflict", ExecutionPolicy::Worker,
		matter_bridge::resolve_project_conflict);
	register_handler("project.sharedActivity", ExecutionPolicy::Worker, matter_bridge::shared_activity);
	register_handler("project.confirmComputer", ExecutionPolicy::Worker, matter_bridge::confirm_computer);
	register_handler("project.revokeComputer", ExecutionPolicy::Worker, matter_bridge::revoke_computer);
	// The firm's team with what its record says of each seat laid over it;
	// this computer takes its own seat's newer profile first, and the firm's
	// AI Setup with it.
	register_handler("project.firmTeam", ExecutionPolicy::Worker, [](const Json &) {
		organisation::adopt_own_profile();
		organisation::adopt_firm_ai_setup();
		const matter::FirmTeam team = organisation::firm_team_with_profiles();
		organisation::refuse_removed_colleagues(team);
		return matter_bridge::firm_team_json(team);
	});
	register_handler("organisation.seatProfiles", ExecutionPolicy::Worker, organisation_bridge::seat_profiles);
	register_handler("organisation.saveSeatProfile", ExecutionPolicy::Worker, organisation_bridge::save_seat_profile);
	register_handler("organisation.setSeatState", ExecutionPolicy::Worker, [this](const Json &params) {
		return organisation_bridge::set_seat_state(params,
			[this](const std::string &seat, const std::string &state, const std::string &password) {
				if (state.empty() == true) {
					if (password.empty() == true) throw std::invalid_argument("A restored colleague signs in with a new password.");
					(void)account_bridge::create_seat(Json{{"seatId", seat}, {"password", password}}, account_services());
				} else {
					(void)account_bridge::remove_seat(Json{{"seatId", seat}}, account_services());
				}
			});
	});
	register_handler("project.resolveOverwrittenChange", ExecutionPolicy::Worker, matter_bridge::resolve_overwritten_change);
    register_handler("project.saveAiSetup", ExecutionPolicy::Worker, matter_bridge::save_ai_setup);

    register_handler("matter.runTask", ExecutionPolicy::Worker, context_bridge::run_task);

    // Master Spec Sec.54 "What leaves this device?" (Phase 08 WP8-B) - never
    // persists anything, makes no provider/network call; see
    // preview_outbound_request()'s doc comment.
    register_handler("matter.previewOutboundRequest", ExecutionPolicy::Worker,
                     context_bridge::preview_outbound_request);

    // Phase 11 (Privacy - outbound Guard and operation detail) "Reveal
    // original locally" - see reveal_protected_occurrence()'s doc comment.
    // Never persists anything and never returns a full Restoration Map, only
    // the one occurrence asked for.
    register_handler("matter.revealProtectedOccurrence", ExecutionPolicy::Worker,
                     context_bridge::reveal_protected_occurrence);

    // Phase 11 explicit preview-session teardown - see
    // close_outbound_preview()'s doc comment.
    register_handler("matter.closeOutboundPreview", ExecutionPolicy::Worker,
                     context_bridge::close_outbound_preview);

    // Serves both Retry (Master Spec Sec.66) and Run with another model
    // (Sec.67) - see retry_matter_task()'s doc comment for how the optional
    // providerId/modelId pair distinguishes them.
    register_handler("matter.retryTask", ExecutionPolicy::Worker, context_bridge::retry_task);

    // Phase 27's Message revision / "Edit and continue" - see
    // revise_task()'s doc comment.
    register_handler("matter.reviseTask", ExecutionPolicy::Worker, context_bridge::revise_task);

    // Master Spec Sec.68 (Consilium) - 2+ connected models independently
    // receiving the same logically equivalent Protected task; see
    // run_consilium()'s doc comment for how independence is enforced.
    register_handler("matter.runConsilium", ExecutionPolicy::Worker,
                     context_bridge::run_consilium);

    register_handler("matter.synthesizeConsilium", ExecutionPolicy::Worker,
                     context_bridge::synthesize_consilium);

    // Master Spec Sec.69 (Completor) - the backend-owned, versioned
    // WorkflowDefinition registry; see list_workflows()/run_workflow().
    register_handler("matter.listWorkflows", ExecutionPolicy::Worker,
                     context_bridge::list_workflows);

    register_handler("matter.runWorkflow", ExecutionPolicy::Worker, context_bridge::run_workflow);

    // Master Spec Sec.24 (local export) - opens the native Save dialog, so
    // like matter.addDocument/revealDocument this must run on the UI thread.
    register_handler("matter.exportText", ExecutionPolicy::UiThread,
                     [this](const Json &params) { return export_matter_text(params); });

    // A Word document or Excel workbook the viewers built from a result: the
    // person chooses where it goes in the native Save dialog (UI thread).
    register_handler("dialog.saveGeneratedFile", ExecutionPolicy::UiThread,
                     [this](const Json &params) { return save_generated_file(params); });

    // Add CPU- or IO-bound methods with ExecutionPolicy::Worker.
    // System dialogs and APIs that need the main thread use UiThread.
  }

  Json add_matter_document(const Json &params) {
    document_bridge::validate_import(params);
    return document_bridge::import_files(params, dialogs_.choose_files());
  }

  // Master Spec Sec.24: local Markdown/plain-text export of an already-
  // persisted MatterTask result or Matter Note. `format` only ever chooses
  // the suggested filename/dialog filter - the written bytes are the exact
  // UTF-8 content already stored (matters::resolve_export_content()),
  // byte-for-byte, for both formats. No provider call, no retrieval, no
  // Papinian operation, no revalidation - purely a local file write.
  // Cancelling the save dialog resolves false, not an error.
  Json export_matter_text(const Json &params) {
    const auto prepared = work_product_bridge::prepare_export(params);
    return work_product_bridge::finish_export(
        prepared, dialogs_.choose_save_path(prepared.default_name, prepared.filter_name,
                                            prepared.extension));
  }

  // The bytes are validated before the dialog opens, so a file Singularity would
  // refuse never asks the person where to put it.
  Json save_generated_file(const Json &params) {
    const generated_file_bridge::PreparedFile prepared = generated_file_bridge::prepare(params);
    return generated_file_bridge::finish(
        prepared, dialogs_.choose_save_path(prepared.default_name, prepared.filter_name,
                                            prepared.extension));
  }

  Json generate_project_usage_statement(const Json& params) {
    const usage_bridge::PreparedStatement prepared = usage_bridge::prepare_statement(params);
    return usage_bridge::finish_statement(prepared, dialogs_.choose_folder());
  }

  // Built and signed from this computer's records before the folder is asked
  // for, so a refusal never asks the lawyer where to put it.
  Json save_work_statement(const Json& params) {
    const usage_bridge::PreparedWorkStatement prepared = usage_bridge::prepare_work_statement(params);
    return usage_bridge::finish_work_statement(prepared, dialogs_.choose_folder());
  }

  Json build_hash() {
    std::call_once(build_hash_once_, [this] {
      build_hash_value_ =
          platform::sha256_hex_digest_of_file(platform::executable_path());
    });
    return Json{{"algorithm", "sha256"}, {"value", build_hash_value_}};
  }

  Json complete_onboarding() {
    const auto status = node_bridge::complete_onboarding(Json::object());

    window_.clear_size_constraints();
    window_.set_minimum_size(app::kMainWindowMinimumSize.width,
                             app::kMainWindowMinimumSize.height);
    window_.set_size(app::kMainWindowInitialSize.width,
                     app::kMainWindowInitialSize.height);

    return status;
  }

  Json runtime_info() const {
    std::vector<std::string> capabilities;
    capabilities.reserve(handlers_.size());
    for (const auto &[name, _] : handlers_) {
      capabilities.push_back(name);
    }
    std::sort(capabilities.begin(), capabilities.end());
#if defined(NDEBUG)
    constexpr const char *build = "Release";
#else
    constexpr const char *build = "Debug";
#endif
    return Json{{"apiVersion", kNativeApiVersion},
                {"appVersion", NATIVE_APP_VERSION},
                {"build", build},
                {"capabilities", capabilities},
                {"platform", platform::contract_platform_name()}};
  }

  void handle_request(const std::string &request_id,
                      const std::string &request) {
    try {
      const auto arguments = Json::parse(request);
      if (!arguments.is_array() || arguments.empty() ||
          !arguments.front().is_string()) {
        throw std::invalid_argument("Expected [method, params]");
      }

      const auto method = arguments.front().get<std::string>();
      // The interface's own half of the diagnostic trace. Not a product
      // capability and not in the native contract: it exists only while
      // SINGULARITY_TRACE_FILE is set, and only ever appends timing records.
      if (method == "__trace") {
        if (platform::trace::enabled() && arguments.size() > 1) record_interface_trace(arguments[1]);
        window_.resolve(request_id, 0, "null");
        return;
      }
      const auto found = handlers_.find(method);
      if (found == handlers_.end()) {
        reject(request_id, nativeErrorFor("native.method_not_found", "Unknown native method: " + method));
        return;
      }

      const auto params = arguments.size() > 1 ? arguments[1] : Json::object();
      const auto handler = found->second;
      const auto requested = std::chrono::steady_clock::now();
      const std::size_t request_bytes = request.size();
      if (handler.policy == ExecutionPolicy::UiThread && handler.continuation) {
        // Only the dialog runs here; what it chose goes to a worker.
        Json chosen;
        try {
          chosen = handler.callback(params);
        } catch (...) {
          reject(request_id, nativeErrorOf(std::current_exception()));
          return;
        }
        executor_.submit([this, request_id, method, requested, request_bytes, continuation = handler.continuation,
                          chosen] { execute(request_id, method, "worker", requested, request_bytes, continuation, chosen); });
      } else if (handler.policy == ExecutionPolicy::UiThread) {
        execute(request_id, method, "ui", requested, request_bytes, handler.callback, params);
      } else {
        BackgroundExecutor &pool = holds_a_run(method) ? runs_ : is_operation_control(method) ? controls_ : executor_;
        pool.submit([this, request_id, method, requested, request_bytes, callback = handler.callback,
                     params] { execute(request_id, method, "worker", requested, request_bytes, callback, params); });
      }
    } catch (const std::exception &error) {
      // The envelope itself could not be read: nothing was run.
      reject(request_id, nativeErrorFor("native.invalid_request", error.what()));
    }
  }

  // Starts the run that carries on a decision made on a kept pause. The answer
  // is already on record, so a run that cannot start now is offered as Resume.
  void resume_paused_decision(const Json &params) {
    const std::string projectId = params.value("projectId", std::string());
    const std::string operationId = params.value("operationId", std::string());
    if (projectId.empty() || operationId.empty()) return;
    if (chat::paused_decision_awaits_resume(projectId, operationId) == false) return;
    try {
      runs_.submit([projectId, operationId] {
        try {
          (void)chat::resume_chat_operation(projectId, operationId);
        } catch (const std::exception &) {
          // Its outcome is on the operation's own record; nobody waits here.
        }
      });
    } catch (const std::exception &) {
    }
  }

  static void record_interface_trace(const Json &payload) {
    if (!payload.is_object() || !payload.contains("entries") || !payload.at("entries").is_array()) return;
    for (const Json &entry : payload.at("entries")) {
      if (!entry.is_object()) continue;
      const std::string kind = entry.value("kind", std::string());
      platform::trace::Fields fields;
      fields.emplace_back("uiAtMs", entry.value("at", 0.0));
      if (kind == "call") fields.emplace_back("ok", entry.value("ok", true));
      if (entry.contains("fields") && entry.at("fields").is_object()) {
        for (const auto &field : entry.at("fields").items()) {
          if (field.value().is_string()) fields.emplace_back(field.key(), field.value().get<std::string>());
          else if (field.value().is_boolean()) fields.emplace_back(field.key(), field.value().get<bool>());
          else if (field.value().is_number()) fields.emplace_back(field.key(), field.value().get<double>());
        }
      }
      platform::trace::record(kind == "call" ? "ui.call" : "ui.mark", entry.value("name", std::string()), fields,
          kind == "call" ? entry.value("ms", 0.0) : -1.0);
    }
  }

  // Runs one native call and answers it. With the diagnostic trace on, every
  // call leaves one record: how long it waited for a worker, how long its
  // handler ran, how large its answer was, and how it ended - never its
  // parameters or its answer.
  void execute(const std::string &request_id, const std::string &method, const char *policy,
               std::chrono::steady_clock::time_point requested, std::size_t request_bytes,
               const std::function<Json(const Json &)> &callback,
               const Json &params) {
    const bool traced = platform::trace::enabled();
    const auto started = std::chrono::steady_clock::now();
    if (traced && std::string_view(policy) == "worker") platform::trace::set_thread_label("bridge-worker");
    std::string outcome = "ok";
    std::size_t result_bytes = 0;
    double handler_ms = 0.0;
    try {
      const Json result = callback(params);
      handler_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      const std::string serialized = result.dump();
      result_bytes = serialized.size();
      window_.resolve(request_id, 0, serialized);
    } catch (...) {
      // Every failure leaves with a stable code and a sentence for the lawyer
      // (NativeError.hpp); what was recorded about it goes with them.
      const NativeError error = nativeErrorOf(std::current_exception());
      outcome = error.code;
      reject(request_id, error);
    }
    if (!traced) return;
    const auto finished = std::chrono::steady_clock::now();
    if (handler_ms == 0.0) handler_ms = std::chrono::duration<double, std::milli>(finished - started).count();
    record_bridge_call(method, policy, std::chrono::duration<double, std::milli>(started - requested).count(),
        handler_ms, request_bytes, result_bytes, outcome,
        std::chrono::duration<double, std::milli>(finished - requested).count());
  }

  void reject(const std::string &request_id, const NativeError &error) {
    window_.resolve(request_id, 1, nativeErrorJson(error).dump());
  }

  // The account bridge on this platform's HTTPS client, credential store and
  // model downloads.
  account_bridge::AccountServices account_services() {
    account_bridge::AccountServices services;
    services.post = [](const std::string &url, const std::string &body) {
      return platform::https_post(url, {{"Content-Type", "application/json"}}, body, 30);
    };
    services.start_authorization = [this](std::string password, std::string challenge_id) {
      downloads_.submit([token = package_downloads_, password = std::move(password),
                         challenge_id = std::move(challenge_id)]() mutable {
        const platform::HttpCancelScope downloads(token);
        try {
          local_models::production_model_package_manager().authorize_and_sync(password, challenge_id);
        } catch (const std::exception &) {
          // The protection model's own status holds what went wrong and the way on.
        }
        password.assign(password.size(), '\0');
        password.clear();
        try {
          if (local_models::production_model_package_manager().installed_and_verified(
                  local_models::papinian_model_id()) == true) {
            routing::papinian_warmup_tracker().run_once();
            routing::load_papinian_model();
          }
        } catch (const std::exception &) {
        }
      });
    };
    services.has_session = [] { return local_models::production_model_package_manager().has_session(); };
    services.session_post = [](const std::string &url, const std::string &body) {
      std::vector<platform::HttpHeader> headers = local_models::production_model_package_manager().session_headers();
      if (headers.empty() == true) return platform::HttpResponse{401, "", {}};
      headers.push_back({"Content-Type", "application/json"});
      return platform::https_post(url, headers, body, 30);
    };
    services.forget_session = [] { local_models::production_model_package_manager().forget_session(); };
    // The recovery codes: saved where the person chooses, or opened as a page
    // to print.
    services.choose_save_path = [this](const std::string &default_name) {
      return dialogs_.choose_save_path(default_name, "Text", "txt");
    };
    services.open_document = [](const std::string &path) { return platform::open_in_default_application(path); };
    services.seat_password_store = [] { return platform::seat_password_store::kind(); };
    services.save_seat_password = [](const std::string &target, const std::string &user, const std::string &password) {
      return platform::seat_password_store::save(target, user, password);
    };
    services.read_seat_password = [](const std::string &target) { return platform::seat_password_store::read(target); };
    services.forget_seat_password = [](const std::string &target) { return platform::seat_password_store::forget(target); };
    // A report or a work log of up to 24 MB: five minutes to go on a slow line.
    services.problem_post = [](const std::string &url, const std::string &content_type, const std::string &body) {
      std::vector<platform::HttpHeader> headers = local_models::production_model_package_manager().session_headers();
      if (headers.empty() == true) return platform::HttpResponse{401, "", {}};
      headers.push_back({"Content-Type", content_type});
      return platform::https_post(url, headers, body, 300);
    };
    services.app_version = NATIVE_APP_VERSION;
    services.platform_name = platform::contract_platform_name();
    services.device_public_key = [] { return platform::device_public_key(); };
    services.sign_with_device = [](const std::string &message) { return platform::sign_with_device_key(message); };
    return services;
  }

  app::AppWindow &window_;
  platform::NativeDialogs dialogs_;
  // The folders chosen for new Matters, by the token the window holds
  // (matter.chooseNewFolder); each used once by project.create.
  std::mutex chosen_folders_mutex_;
  std::map<std::string, std::filesystem::path> chosen_folders_;
  BackgroundExecutor executor_;
  // Requests that hold a whole run (holds_a_run()).
  BackgroundExecutor runs_;
  // Stop, approvals, answers and the card's status read (is_operation_control()).
  BackgroundExecutor controls_;
  // Model package downloads a sign-in starts: one at a time, never on a
  // thread a chat run or a window read needs.
  BackgroundExecutor downloads_;
  // Stops every model package download in flight when Singularity closes.
  std::shared_ptr<platform::HttpCancelToken> package_downloads_ = std::make_shared<platform::HttpCancelToken>();
  std::unordered_map<std::string, Handler> handlers_;
  std::once_flag build_hash_once_;
  std::string build_hash_value_;
  // Whether events may still reach the window (Platform/Events).
  std::shared_ptr<EventGate> event_gate_ = std::make_shared<EventGate>();
  // The other carriers' event listeners (the Office connection).
  std::shared_ptr<EventListeners> event_listeners_ = std::make_shared<EventListeners>();
  // Whether shutdown() has begun.
  std::atomic<bool> shut_down_{false};
};

NativeBridge::NativeBridge(app::AppWindow &window, bool start_background_services)
    : impl_(std::make_unique<Impl>(window, start_background_services)) {}

NativeBridge::~NativeBridge() { impl_->shutdown(); }

void NativeBridge::shutdown() noexcept { impl_->shutdown(); }

void NativeBridge::emit(const std::string &event_name,
                        const std::string &json_payload) {
  impl_->emit(event_name, json_payload);
}

void NativeBridge::install() { impl_->install(); }

std::string NativeBridge::invoke_worker(const std::string &method,
	                                    const std::string &json_params) {
	return impl_->invoke_worker(method, json_params);
}

NativeBridge::CarriedAnswer NativeBridge::answer_carried_call(const std::string &method, const std::string &json_params) {
	return impl_->answer_carried_call(method, json_params);
}

int NativeBridge::add_event_listener(std::function<void(const std::string &, const std::string &)> listener) {
	return impl_->add_event_listener(std::move(listener));
}

void NativeBridge::remove_event_listener(int handle) {
	impl_->remove_event_listener(handle);
}

std::string NativeBridge::page_script() const {
	return impl_->page_script();
}

} // namespace singularity::bridge
