#include "CommandsSunBoxDevtoolsInterface.h"
#include "USBHostDriver.h"
#include "HIDMouseDescriptorHandler.h"
#include "SunBoxEEPROM.h"
#include "SunBoxStartup.h"
#include "SunBoxLogger.h"

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
        logger.infof("Unknown command: %s", baseCommand.c_str());
        logger.info("Type 'help' for available commands");
    }
}

void CommandsSunBoxDevtoolsInterface::handleHelp() {
    logger.info("=== SunBox DevTools Commands ===");
    logger.info("help           - Show this help");
    logger.info("status         - Show system status");
    logger.info("debug          - Toggle debug mode (persistent)");
    logger.info("dump           - Dump USB device descriptors");
    logger.info("claimcorrection vid,pid,interface,endpoint - Force specific interface");
    logger.info("               Example: claimcorrection 046d,c53f,1,82");
    logger.info("claimclear     - Clear forced interface configuration");
    logger.info("=================================");
}

void CommandsSunBoxDevtoolsInterface::handleStatus() {
    logger.info("=== System Status ===");
    
    // USB Device status
    logger.info("USB Device: ");
    if (usbHostDriver && usbHostDriver->isReady()) {
        logger.infof("Connected (VID:0x%04X PID:0x%04X)", 
                    usbHostDriver->getVendorID(), 
                    usbHostDriver->getProductID());
    } else {
        logger.info("Not connected");
    }
    
    // HID Handler status
    logger.info("HID Handler: ");
    if (hidHandler && hidHandler->isReady()) {
        logger.infof("Ready (Interface %d, EP 0x%02X)",
                    hidHandler->getInterfaceNumber(),
                    hidHandler->getEndpointAddress() | 0x80);
    } else {
        logger.info("Not ready");
    }
    
    // Debug mode
    logger.infof("Debug Mode: %s (persistent)", debugEnabled ? "ON" : "OFF");
    
    // Force claim config
    ClaimConfig config;
    if (sunboxEEPROM.loadClaimConfig(config)) {
        logger.infof("Claim Correction: VID=0x%04X PID=0x%04X Interface=%d Endpoint=0x%02X",
                    config.vid, config.pid, config.interface_num, config.endpoint_addr);
    } else {
        logger.info("Claim Correction: Not configured");
    }
    
    logger.info("====================");
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
    
    logger.infof("Debug mode %s (saved to EEPROM)", debugEnabled ? "ON" : "OFF");
}

void CommandsSunBoxDevtoolsInterface::handleDump() {
    if (usbHostDriver) {
        usbHostDriver->dumpDeviceInfo();
    } else {
        logger.info("USB Host Driver not available");
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
        logger.info("Invalid format! Use: claimcorrection vid,pid,interface,endpoint");
        logger.info("Example: claimcorrection 046d,c53f,1,82");
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
        logger.info("Claim correction configuration saved:");
        logger.infof("VID=0x%04X PID=0x%04X Interface=%d Endpoint=0x%02X",
                    vid, pid, iface, ep);
        logger.info("Configuration will be used on next device connection");
    } else {
        logger.info("Failed to save configuration");
    }
}

void CommandsSunBoxDevtoolsInterface::handleClaimClear() {
    sunboxEEPROM.clearClaimConfig();
    logger.info("Claim correction configuration cleared");
}