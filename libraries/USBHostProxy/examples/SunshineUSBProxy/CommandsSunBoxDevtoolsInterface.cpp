#include "CommandsSunBoxDevtoolsInterface.h"
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"
#include "SunBoxEEPROM.h"
#include "SunBoxStartup.h"

CommandsSunBoxDevtoolsInterface::CommandsSunBoxDevtoolsInterface()
    : usbHostDriver(nullptr), hidHandler(nullptr), debugEnabled(false) {
}

void CommandsSunBoxDevtoolsInterface::begin() {
    // Get current debug state from SunBoxStartup (already loaded at startup)
    debugEnabled = SunBoxStartup::isDebugEnabled();
}

void CommandsSunBoxDevtoolsInterface::handleCommand(const String& cmd) {
    String command = cmd;
    command.trim();
    if (command.length() == 0) return;
    
    // Parse command and arguments
    int spaceIndex = command.indexOf(' ');
    String baseCommand = (spaceIndex > 0) ? command.substring(0, spaceIndex) : command;
    String args = (spaceIndex > 0) ? command.substring(spaceIndex + 1) : "";
    
    // Handle commands
    if (baseCommand == "help" || baseCommand == "?") {
        handleHelp();
    }
    else if (baseCommand == "status") {
        handleStatus();
    }
    else if (baseCommand == "debug") {
        handleDebug();
    }
    else if (baseCommand == "dump") {
        handleDump();
    }
    else if (baseCommand == "claimcorrection") {
        handleClaimCorrection(args);
    }
    else if (baseCommand == "claimclear") {
        handleClaimClear();
    }
    else {
        Serial4.print("S: Unknown command: ");
        Serial4.println(baseCommand);
        Serial4.println("S: Type 'help' for available commands");
    }
}

void CommandsSunBoxDevtoolsInterface::handleHelp() {
    Serial4.println("\nI: === SunBox DevTools Commands ===");
    Serial4.println("I: help           - Show this help");
    Serial4.println("I: status         - Show system status");
    Serial4.println("I: debug          - Toggle debug mode (persistent)");
    Serial4.println("I: dump           - Dump USB device descriptors");
    Serial4.println("I: claimcorrection vid,pid,interface,endpoint - Force specific interface");
    Serial4.println("I:                Example: claimcorrection 046d,c53f,1,82");
    Serial4.println("I: claimclear     - Clear forced interface configuration");
    Serial4.println("I: =================================");
}

void CommandsSunBoxDevtoolsInterface::handleStatus() {
    Serial4.println("\nI: === System Status ===");
    
    // USB Device status
    Serial4.print("I: USB Device: ");
    if (usbHostDriver && usbHostDriver->isReady()) {
        Serial4.print("Connected (VID:0x");
        Serial4.print(usbHostDriver->getVendorID(), HEX);
        Serial4.print(" PID:0x");
        Serial4.print(usbHostDriver->getProductID(), HEX);
        Serial4.println(")");
    } else {
        Serial4.println("Not connected");
    }
    
    // HID Handler status
    Serial4.print("I: HID Handler: ");
    if (hidHandler && hidHandler->isReady()) {
        Serial4.print("Ready (Interface ");
        Serial4.print(hidHandler->getInterfaceNumber());
        Serial4.print(", EP 0x");
        Serial4.print(hidHandler->getEndpointAddress() | 0x80, HEX);
        Serial4.println(")");
    } else {
        Serial4.println("Not ready");
    }
    
    // Debug mode
    Serial4.print("I: Debug Mode: ");
    Serial4.print(debugEnabled ? "ON" : "OFF");
    Serial4.println(" (persistent)");
    
    // Force claim config
    ClaimConfig config;
    if (sunboxEEPROM.loadClaimConfig(config)) {
        Serial4.print("I: Claim Correction: VID=0x");
        Serial4.print(config.vid, HEX);
        Serial4.print(" PID=0x");
        Serial4.print(config.pid, HEX);
        Serial4.print(" Interface=");
        Serial4.print(config.interface_num);
        Serial4.print(" Endpoint=0x");
        Serial4.println(config.endpoint_addr, HEX);
    } else {
        Serial4.println("I: Claim Correction: Not configured");
    }
    
    Serial4.println("I: ====================");
}

void CommandsSunBoxDevtoolsInterface::handleDebug() {
    // Toggle debug mode using EEPROM
    debugEnabled = sunboxEEPROM.toggleDebugMode();
    
    // Update the global debug state in SunBoxStartup
    SunBoxStartup::setDebugEnabled(debugEnabled);
    
    // Update debug mode in components if they exist
    if (hidHandler) {
        hidHandler->setDebugOutput(debugEnabled);
    }
    
    Serial4.print("S: Debug mode ");
    Serial4.print(debugEnabled ? "ON" : "OFF");
    Serial4.println(" (saved to EEPROM)");
}

void CommandsSunBoxDevtoolsInterface::handleDump() {
    if (usbHostDriver) {
        usbHostDriver->dumpDeviceInfo();
    } else {
        Serial4.println("S: USB Host Driver not available");
    }
}

void CommandsSunBoxDevtoolsInterface::handleClaimCorrection(const String& args) {
    // Parse format: vid,pid,interface,endpoint
    int commas[3];
    int commaCount = 0;
    
    for (unsigned int i = 0; i < args.length() && commaCount < 3; i++) {
        if (args.charAt(i) == ',') {
            commas[commaCount++] = i;
        }
    }
    
    if (commaCount != 3) {
        Serial4.println("S: Invalid format! Use: claimcorrection vid,pid,interface,endpoint");
        Serial4.println("S: Example: claimcorrection 046d,c53f,1,82");
        return;
    }
    
    // Parse values
    String vidStr = args.substring(0, commas[0]);
    String pidStr = args.substring(commas[0] + 1, commas[1]);
    String ifaceStr = args.substring(commas[1] + 1, commas[2]);
    String epStr = args.substring(commas[2] + 1);
    
    // Convert to numbers
    uint16_t vid = strtoul(vidStr.c_str(), NULL, 16);
    uint16_t pid = strtoul(pidStr.c_str(), NULL, 16);
    uint8_t iface = strtoul(ifaceStr.c_str(), NULL, 10);
    uint8_t ep = strtoul(epStr.c_str(), NULL, 16);
    
    // Save to EEPROM
    if (sunboxEEPROM.saveClaimConfig(vid, pid, iface, ep)) {
        Serial4.println("S: Claim correction configuration saved:");
        Serial4.print("S: VID=0x");
        Serial4.print(vid, HEX);
        Serial4.print(" PID=0x");
        Serial4.print(pid, HEX);
        Serial4.print(" Interface=");
        Serial4.print(iface);
        Serial4.print(" Endpoint=0x");
        Serial4.println(ep, HEX);
        Serial4.println("S: Configuration will be used on next device connection");
    } else {
        Serial4.println("S: Failed to save configuration");
    }
}

void CommandsSunBoxDevtoolsInterface::handleClaimClear() {
    sunboxEEPROM.clearClaimConfig();
    Serial4.println("S: Claim correction configuration cleared");
}