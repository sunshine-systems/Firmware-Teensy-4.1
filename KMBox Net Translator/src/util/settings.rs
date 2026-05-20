//! Persisted settings utility: loads `config.json` from disk, validates,
//! and surfaces a structured `Settings` value to the rest of the program.

use std::fs;
use std::net::IpAddr;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, bail, Context, Result};
use serde::{Deserialize, Serialize};

pub const CONFIG_FILENAME: &str = "config.json";
pub const DEFAULT_UDP_PORT: u16 = 8888;
pub const DEFAULT_BAUD: u32 = 115200;
pub const DEFAULT_MAC: &str = "01FBC068";

#[derive(Debug, Clone, Serialize, Deserialize)]
struct RawSettings {
    #[serde(default)]
    pub listen_addr: String,
    #[serde(default)]
    pub udp_port: Option<u16>,
    #[serde(default)]
    pub com_port: String,
    #[serde(default)]
    pub baud_rate: Option<u32>,
    #[serde(default)]
    pub device_mac: Option<String>,
}

#[derive(Debug, Clone)]
pub struct Settings {
    pub listen_addr: IpAddr,
    pub udp_port: u16,
    pub com_port: String,
    pub baud_rate: u32,
    pub device_mac: u32,
    pub device_mac_str: String,
}

fn default_json() -> &'static str {
    "{\n  \"listen_addr\": \"\",\n  \"udp_port\": 8888,\n  \"com_port\": \"\",\n  \"baud_rate\": 115200,\n  \"device_mac\": \"01FBC068\"\n}\n"
}

fn parse_mac(s: &str) -> Result<u32> {
    let t = s.trim();
    if t.len() != 8 || !t.chars().all(|c| c.is_ascii_hexdigit()) {
        bail!(
            "device_mac must be exactly 8 hex characters (got {:?})",
            s
        );
    }
    u32::from_str_radix(t, 16).map_err(|e| anyhow!("device_mac hex parse: {}", e))
}

fn validate(raw: RawSettings) -> Result<Settings> {
    if raw.listen_addr.trim().is_empty() {
        bail!("listen_addr is required and must be non-empty");
    }
    let listen_addr: IpAddr = raw
        .listen_addr
        .trim()
        .parse()
        .with_context(|| format!("listen_addr {:?} is not a valid IP address", raw.listen_addr))?;

    let udp_port = match raw.udp_port {
        Some(0) => bail!("udp_port must be 1..=65535"),
        Some(p) => p,
        None => DEFAULT_UDP_PORT,
    };

    if raw.com_port.trim().is_empty() {
        bail!("com_port is required and must be non-empty");
    }
    let com_port = raw.com_port.trim().to_string();

    let baud_rate = match raw.baud_rate {
        Some(0) => bail!("baud_rate must be > 0"),
        Some(b) => b,
        None => DEFAULT_BAUD,
    };

    let mac_str = raw
        .device_mac
        .clone()
        .unwrap_or_else(|| DEFAULT_MAC.to_string());
    let device_mac = parse_mac(&mac_str)?;

    Ok(Settings {
        listen_addr,
        udp_port,
        com_port,
        baud_rate,
        device_mac,
        device_mac_str: mac_str.to_uppercase(),
    })
}

pub enum LoadOutcome {
    /// Config parsed and validated cleanly.
    Loaded(Settings),
    /// File was missing OR structurally unusable (unreadable / not valid JSON).
    /// A fresh default has been written at `path`.
    WroteDefault {
        path: PathBuf,
        reason: Option<String>,
    },
    /// File parsed as JSON but one or more field values are wrong. The file
    /// has NOT been touched — the user's other edits are preserved. They
    /// should fix the specific value the error names and re-run.
    Invalid { path: PathBuf, reason: String },
}

/// Load `config.json` from `dir`. Behaviour:
///   * missing             -> write default, return WroteDefault { reason: None }
///   * unreadable / bad JSON -> rewrite default, return WroteDefault { reason: Some(...) }
///                              (file itself is structurally unusable, nothing to preserve)
///   * parses but value invalid -> LEAVE FILE ALONE, return Invalid { reason }
///                                 (user's edits are kept; they fix the named field)
///   * present and valid   -> return Loaded(settings)
pub fn load_or_create(dir: &Path) -> Result<LoadOutcome> {
    let path = dir.join(CONFIG_FILENAME);

    if !path.exists() {
        fs::write(&path, default_json())
            .with_context(|| format!("writing default config to {}", path.display()))?;
        return Ok(LoadOutcome::WroteDefault { path, reason: None });
    }

    let text = match fs::read_to_string(&path) {
        Ok(s) => s,
        Err(e) => {
            let _ = fs::remove_file(&path);
            fs::write(&path, default_json()).with_context(|| {
                format!("rewriting default config to {}", path.display())
            })?;
            return Ok(LoadOutcome::WroteDefault {
                path,
                reason: Some(format!("could not read file: {}", e)),
            });
        }
    };

    let raw: RawSettings = match serde_json::from_str(&text) {
        Ok(r) => r,
        Err(e) => {
            let _ = fs::remove_file(&path);
            fs::write(&path, default_json()).with_context(|| {
                format!("rewriting default config to {}", path.display())
            })?;
            return Ok(LoadOutcome::WroteDefault {
                path,
                reason: Some(format!("JSON parse error: {}", e)),
            });
        }
    };

    match validate(raw) {
        Ok(settings) => Ok(LoadOutcome::Loaded(settings)),
        Err(e) => Ok(LoadOutcome::Invalid {
            path,
            reason: format!("{}", e),
        }),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mac_ok() {
        assert_eq!(parse_mac("01FBC068").unwrap(), 0x01FBC068);
        assert_eq!(parse_mac("01fbc068").unwrap(), 0x01FBC068);
    }

    #[test]
    fn mac_rejects_bad_len() {
        assert!(parse_mac("01FBC06").is_err());
        assert!(parse_mac("01FBC0688").is_err());
    }

    #[test]
    fn mac_rejects_non_hex() {
        assert!(parse_mac("01FBC0GG").is_err());
    }

    #[test]
    fn validate_requires_listen_and_com() {
        let raw = RawSettings {
            listen_addr: "".into(),
            udp_port: None,
            com_port: "COM7".into(),
            baud_rate: None,
            device_mac: None,
        };
        assert!(validate(raw).is_err());

        let raw = RawSettings {
            listen_addr: "0.0.0.0".into(),
            udp_port: None,
            com_port: "".into(),
            baud_rate: None,
            device_mac: None,
        };
        assert!(validate(raw).is_err());
    }

    #[test]
    fn validate_fills_defaults() {
        let raw = RawSettings {
            listen_addr: "127.0.0.1".into(),
            udp_port: None,
            com_port: "COM3".into(),
            baud_rate: None,
            device_mac: None,
        };
        let s = validate(raw).unwrap();
        assert_eq!(s.udp_port, DEFAULT_UDP_PORT);
        assert_eq!(s.baud_rate, DEFAULT_BAUD);
        assert_eq!(s.device_mac, 0x01FBC068);
    }
}
