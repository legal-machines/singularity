#include "Office/OfficeAddins/OfficeAddins.hpp"

#include <sstream>
#include <vector>

namespace singularity::office {
namespace {

// The words on Office's ribbon: the group is Singularity, its one button
// opens the file's Matter.
constexpr const char *kGroupLabel = "Singularity";
constexpr const char *kButtonLabel = "Matter";
constexpr const char *kButtonTip = "The Matter this file belongs to: who works on it, where the work stands and what waits for you.";
constexpr const char *kMailButtonTip = "The Matter this message belongs to: file its attachments, give the work to a colleague, send what was accepted.";
constexpr const char *kDescription = "The Matter of the file you are in, with its team and its work, from Singularity on this computer.";

std::string url(const std::string &path) {
	return office_origin() + path;
}

// The ribbon resources both manifests share: the icons, the pane and the
// command page, and the words.
std::string resources(const char *tip) {
	std::ostringstream text;
	text << "    <Resources>\n"
	     << "      <bt:Images>\n"
	     << "        <bt:Image id=\"Icon.16\" DefaultValue=\"" << url("/office/assets/singularity-16.png") << "\"/>\n"
	     << "        <bt:Image id=\"Icon.32\" DefaultValue=\"" << url("/office/assets/singularity-32.png") << "\"/>\n"
	     << "        <bt:Image id=\"Icon.80\" DefaultValue=\"" << url("/office/assets/singularity-80.png") << "\"/>\n"
	     << "      </bt:Images>\n"
	     << "      <bt:Urls>\n"
	     << "        <bt:Url id=\"Pane.Url\" DefaultValue=\"" << url("/office/index.html") << "\"/>\n"
	     << "        <bt:Url id=\"Commands.Url\" DefaultValue=\"" << url("/office/commands.html") << "\"/>\n"
	     << "      </bt:Urls>\n"
	     << "      <bt:ShortStrings>\n"
	     << "        <bt:String id=\"Group.Label\" DefaultValue=\"" << kGroupLabel << "\"/>\n"
	     << "        <bt:String id=\"Open.Label\" DefaultValue=\"" << kButtonLabel << "\"/>\n"
	     << "      </bt:ShortStrings>\n"
	     << "      <bt:LongStrings>\n"
	     << "        <bt:String id=\"Open.Tip\" DefaultValue=\"" << tip << "\"/>\n"
	     << "      </bt:LongStrings>\n"
	     << "    </Resources>\n";
	return text.str();
}

std::string icon() {
	return "                <Icon>\n"
	       "                  <bt:Image size=\"16\" resid=\"Icon.16\"/>\n"
	       "                  <bt:Image size=\"32\" resid=\"Icon.32\"/>\n"
	       "                  <bt:Image size=\"80\" resid=\"Icon.80\"/>\n"
	       "                </Icon>\n";
}

// The group on a tab, with its one button that opens the pane. Word's and
// Excel's group has its own icon and names its pane; Outlook's schema allows
// neither (its Action holds only the page). The pane's name is the one Office
// opens with a file that asks for it (Office.AutoShowTaskpaneWithDocument), as
// Singularity's own file does (Office/OfficeApps, D-191): the button's pane
// and that one are one pane.
// A pane that pins (`pins`) stays open as the person moves from one letter to
// the next, and follows the one selected (Outlook, VersionOverrides 1.1).
std::string group(const std::string &tab, const std::string &id, bool is_documents, bool pins = false) {
	std::ostringstream text;
	text << "            <OfficeTab id=\"" << tab << "\">\n"
	     << "              <Group id=\"Singularity." << id << ".Group\">\n"
	     << "                <Label resid=\"Group.Label\"/>\n"
	     << (is_documents ? icon() : std::string())
	     << "                <Control xsi:type=\"Button\" id=\"Singularity." << id << ".Open\">\n"
	     << "                  <Label resid=\"Open.Label\"/>\n"
	     << "                  <Supertip>\n"
	     << "                    <Title resid=\"Open.Label\"/>\n"
	     << "                    <Description resid=\"Open.Tip\"/>\n"
	     << "                  </Supertip>\n"
	     << icon()
	     << "                  <Action xsi:type=\"ShowTaskpane\">\n"
	     << (is_documents ? "                    <TaskpaneId>Office.AutoShowTaskpaneWithDocument</TaskpaneId>\n" : "")
	     << "                    <SourceLocation resid=\"Pane.Url\"/>\n"
	     << (pins ? "                    <SupportsPinning>true</SupportsPinning>\n" : "")
	     << "                  </Action>\n"
	     << "                </Control>\n"
	     << "              </Group>\n"
	     << "            </OfficeTab>\n";
	return text.str();
}

// What every manifest says first, in the order the schema keeps.
std::string head(const char *id, const std::string &version) {
	std::ostringstream text;
	text << "  <Id>" << id << "</Id>\n"
	     << "  <Version>" << version << "</Version>\n"
	     << "  <ProviderName>Legal Machines</ProviderName>\n"
	     << "  <DefaultLocale>en-US</DefaultLocale>\n"
	     << "  <DisplayName DefaultValue=\"Singularity\"/>\n"
	     << "  <Description DefaultValue=\"" << kDescription << "\"/>\n"
	     << "  <IconUrl DefaultValue=\"" << url("/office/assets/singularity-32.png") << "\"/>\n"
	     << "  <HighResolutionIconUrl DefaultValue=\"" << url("/office/assets/singularity-64.png") << "\"/>\n"
	     << "  <SupportUrl DefaultValue=\"" << url("/office/index.html") << "\"/>\n"
	     << "  <AppDomains>\n"
	     << "    <AppDomain>" << office_origin() << "</AppDomain>\n"
	     << "  </AppDomains>\n";
	return text.str();
}

} // namespace

std::string office_origin() {
	return "https://localhost:" + std::to_string(kOfficePort);
}

std::string manifest_version(const std::string &app_version) {
	// Office takes no version below 1.0: Singularity's "major.minor.patch"
	// is written after a leading 1, so every release still counts up.
	std::vector<std::string> parts{"1"};
	std::string part;
	for (const char character : app_version) {
		if (character == '.') {
			parts.push_back(part);
			part.clear();
		} else if (character >= '0' && character <= '9') {
			part += character;
		} else {
			break;
		}
	}
	parts.push_back(part);
	while (parts.size() < 4) parts.push_back("0");
	parts.resize(4);
	std::string version;
	for (std::size_t index = 0; index < parts.size(); ++index) {
		if (index > 0) version += '.';
		version += parts[index].empty() ? "0" : parts[index];
	}
	return version;
}

std::string documents_manifest(const std::string &version) {
	std::ostringstream text;
	text << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	     << "<OfficeApp xmlns=\"http://schemas.microsoft.com/office/appforoffice/1.1\"\n"
	     << "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
	     << "  xmlns:bt=\"http://schemas.microsoft.com/office/officeappbasictypes/1.0\"\n"
	     << "  xmlns:ov=\"http://schemas.microsoft.com/office/taskpaneappversionoverrides\"\n"
	     << "  xsi:type=\"TaskPaneApp\">\n"
	     << head(kDocumentsAddinId, version)
	     << "  <Hosts>\n"
	     << "    <Host Name=\"Document\"/>\n"
	     << "    <Host Name=\"Workbook\"/>\n"
	     << "    <Host Name=\"Presentation\"/>\n"
	     << "  </Hosts>\n"
	     << "  <DefaultSettings>\n"
	     << "    <SourceLocation DefaultValue=\"" << url("/office/index.html") << "\"/>\n"
	     << "  </DefaultSettings>\n"
	     << "  <Permissions>ReadWriteDocument</Permissions>\n"
	     << "  <VersionOverrides xmlns=\"http://schemas.microsoft.com/office/taskpaneappversionoverrides\" xsi:type=\"VersionOverridesV1_0\">\n"
	     << "    <Hosts>\n";
	for (const char *host : {"Document", "Workbook", "Presentation"}) {
		text << "      <Host xsi:type=\"" << host << "\">\n"
		     << "        <DesktopFormFactor>\n"
		     << "          <FunctionFile resid=\"Commands.Url\"/>\n"
		     << "          <ExtensionPoint xsi:type=\"PrimaryCommandSurface\">\n"
		     << group("TabHome", host, true)
		     << "          </ExtensionPoint>\n"
		     << "        </DesktopFormFactor>\n"
		     << "      </Host>\n";
	}
	text << "    </Hosts>\n"
	     << resources(kButtonTip)
	     << "  </VersionOverrides>\n"
	     << "</OfficeApp>\n";
	return text.str();
}

std::string outlook_manifest(const std::string &version) {
	std::ostringstream text;
	text << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	     << "<OfficeApp xmlns=\"http://schemas.microsoft.com/office/appforoffice/1.1\"\n"
	     << "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
	     << "  xmlns:bt=\"http://schemas.microsoft.com/office/officeappbasictypes/1.0\"\n"
	     << "  xmlns:mailappor=\"http://schemas.microsoft.com/office/mailappversionoverrides/1.0\"\n"
	     << "  xsi:type=\"MailApp\">\n"
	     << head(kOutlookAddinId, version)
	     << "  <Hosts>\n"
	     << "    <Host Name=\"Mailbox\"/>\n"
	     << "  </Hosts>\n"
	     << "  <Requirements>\n"
	     << "    <Sets>\n"
	     << "      <Set Name=\"Mailbox\" MinVersion=\"1.1\"/>\n"
	     << "    </Sets>\n"
	     << "  </Requirements>\n"
	     << "  <FormSettings>\n"
	     << "    <Form xsi:type=\"ItemRead\">\n"
	     << "      <DesktopSettings>\n"
	     << "        <SourceLocation DefaultValue=\"" << url("/office/index.html") << "\"/>\n"
	     << "        <RequestedHeight>360</RequestedHeight>\n"
	     << "      </DesktopSettings>\n"
	     << "    </Form>\n"
	     << "  </FormSettings>\n"
	     << "  <Permissions>ReadWriteItem</Permissions>\n"
	     << "  <Rule xsi:type=\"RuleCollection\" Mode=\"Or\">\n"
	     << "    <Rule xsi:type=\"ItemIs\" ItemType=\"Message\" FormType=\"Read\"/>\n"
	     << "    <Rule xsi:type=\"ItemIs\" ItemType=\"Message\" FormType=\"Edit\"/>\n"
	     << "  </Rule>\n"
	     << "  <DisableEntityHighlighting>false</DisableEntityHighlighting>\n"
	     << "  <VersionOverrides xmlns=\"http://schemas.microsoft.com/office/mailappversionoverrides\" xsi:type=\"VersionOverridesV1_0\">\n"
	     << "    <Requirements>\n"
	     << "      <bt:Sets DefaultMinVersion=\"1.3\">\n"
	     << "        <bt:Set Name=\"Mailbox\"/>\n"
	     << "      </bt:Sets>\n"
	     << "    </Requirements>\n"
	     << "    <Hosts>\n"
	     << "      <Host xsi:type=\"MailHost\">\n"
	     << "        <DesktopFormFactor>\n"
	     << "          <FunctionFile resid=\"Commands.Url\"/>\n"
	     << "          <ExtensionPoint xsi:type=\"MessageReadCommandSurface\">\n"
	     << group("TabDefault", "Read", false)
	     << "          </ExtensionPoint>\n"
	     << "          <ExtensionPoint xsi:type=\"MessageComposeCommandSurface\">\n"
	     << group("TabDefault", "Compose", false)
	     << "          </ExtensionPoint>\n"
	     << "        </DesktopFormFactor>\n"
	     << "      </Host>\n"
	     << "    </Hosts>\n"
	     << resources(kMailButtonTip)
	     // Where Outlook has Mailbox 1.5, the pane pins and follows the letter
	     // selected; elsewhere the 1.0 overrides above stand.
	     << "    <VersionOverrides xmlns=\"http://schemas.microsoft.com/office/mailappversionoverrides/1.1\" xsi:type=\"VersionOverridesV1_1\">\n"
	     << "      <Requirements>\n"
	     << "        <bt:Sets DefaultMinVersion=\"1.5\">\n"
	     << "          <bt:Set Name=\"Mailbox\"/>\n"
	     << "        </bt:Sets>\n"
	     << "      </Requirements>\n"
	     << "      <Hosts>\n"
	     << "        <Host xsi:type=\"MailHost\">\n"
	     << "          <DesktopFormFactor>\n"
	     << "            <FunctionFile resid=\"Commands.Url\"/>\n"
	     << "            <ExtensionPoint xsi:type=\"MessageReadCommandSurface\">\n"
	     << group("TabDefault", "Read", false, true)
	     << "            </ExtensionPoint>\n"
	     << "            <ExtensionPoint xsi:type=\"MessageComposeCommandSurface\">\n"
	     << group("TabDefault", "Compose", false)
	     << "            </ExtensionPoint>\n"
	     << "          </DesktopFormFactor>\n"
	     << "        </Host>\n"
	     << "      </Hosts>\n"
	     << resources(kMailButtonTip)
	     << "    </VersionOverrides>\n"
	     << "  </VersionOverrides>\n"
	     << "</OfficeApp>\n";
	return text.str();
}

} // namespace singularity::office
