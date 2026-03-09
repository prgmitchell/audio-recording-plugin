#ifndef PRODUCT_NAME
  #error PRODUCT_NAME is required
#endif
#ifndef PRODUCT_VERSION
  #error PRODUCT_VERSION is required
#endif
#ifndef PRODUCT_VERSION_WIN
  #error PRODUCT_VERSION_WIN is required
#endif
#ifndef PRODUCT_AUTHOR
  #define PRODUCT_AUTHOR "Unknown"
#endif
#ifndef PRODUCT_WEBSITE
  #define PRODUCT_WEBSITE "https://example.com"
#endif
#ifndef PLUGIN_DIR_NAME
  #error PLUGIN_DIR_NAME is required
#endif
#ifndef PLUGIN_PAYLOAD_DIR
  #error PLUGIN_PAYLOAD_DIR is required
#endif
#ifndef OUTPUT_DIR
  #error OUTPUT_DIR is required
#endif
#ifndef OUTPUT_BASENAME
  #error OUTPUT_BASENAME is required
#endif
#if PRODUCT_NAME == "ProductName"
  #error PRODUCT_NAME placeholder value detected
#endif
#if PRODUCT_VERSION == "ProductVersion"
  #error PRODUCT_VERSION placeholder value detected
#endif

[Setup]
AppId={{D34A0A7A-5EE4-4279-BD57-7B9C2A7DAB8C}
AppName={#PRODUCT_NAME}
AppVersion={#PRODUCT_VERSION}
AppVerName={#PRODUCT_NAME} {#PRODUCT_VERSION}
AppPublisher={#PRODUCT_AUTHOR}
AppPublisherURL={#PRODUCT_WEBSITE}
AppSupportURL={#PRODUCT_WEBSITE}
AppUpdatesURL={#PRODUCT_WEBSITE}
UninstallDisplayName={#PRODUCT_NAME} {#PRODUCT_VERSION}
DefaultDirName={commonappdata}\obs-studio\plugins\{#PLUGIN_DIR_NAME}
DirExistsWarning=no
DisableProgramGroupPage=yes
OutputDir={#OUTPUT_DIR}
OutputBaseFilename={#OUTPUT_BASENAME}
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
VersionInfoCompany={#PRODUCT_AUTHOR}
VersionInfoDescription={#PRODUCT_NAME} installer
VersionInfoProductName={#PRODUCT_NAME}
VersionInfoProductVersion={#PRODUCT_VERSION_WIN}
VersionInfoVersion={#PRODUCT_VERSION_WIN}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#PLUGIN_PAYLOAD_DIR}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
