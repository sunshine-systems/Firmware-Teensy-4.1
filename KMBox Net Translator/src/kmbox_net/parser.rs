//! Parsing of incoming KMBox Net packets and construction of replies.

use anyhow::{bail, Result};
use byteorder::{ByteOrder, LittleEndian};

use super::schema::{
    Header, SoftKeyboard, SoftMouse, HEADER_LEN, SOFT_KEYBOARD_LEN, SOFT_MOUSE_LEN,
};

impl Header {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < HEADER_LEN {
            bail!(
                "packet too short for header: {} < {}",
                bytes.len(),
                HEADER_LEN
            );
        }
        Ok(Header {
            mac: LittleEndian::read_u32(&bytes[0..4]),
            rand: LittleEndian::read_u32(&bytes[4..8]),
            indexpts: LittleEndian::read_u32(&bytes[8..12]),
            cmd: LittleEndian::read_u32(&bytes[12..16]),
        })
    }

    /// Build the reply header: same mac, echo rand, `indexpts + 1`, same cmd.
    pub fn reply(&self) -> [u8; HEADER_LEN] {
        let mut out = [0u8; HEADER_LEN];
        LittleEndian::write_u32(&mut out[0..4], self.mac);
        LittleEndian::write_u32(&mut out[4..8], self.rand);
        LittleEndian::write_u32(&mut out[8..12], self.indexpts.wrapping_add(1));
        LittleEndian::write_u32(&mut out[12..16], self.cmd);
        out
    }
}

impl SoftMouse {
    pub fn parse(body: &[u8]) -> Result<Self> {
        if body.len() < SOFT_MOUSE_LEN {
            bail!(
                "soft_mouse body too short: {} < {}",
                body.len(),
                SOFT_MOUSE_LEN
            );
        }
        let button = LittleEndian::read_i32(&body[0..4]);
        let x = LittleEndian::read_i32(&body[4..8]);
        let y = LittleEndian::read_i32(&body[8..12]);
        let wheel = LittleEndian::read_i32(&body[12..16]);
        let mut point = [0i32; 10];
        for (i, p) in point.iter_mut().enumerate() {
            let off = 16 + i * 4;
            *p = LittleEndian::read_i32(&body[off..off + 4]);
        }
        Ok(SoftMouse {
            button,
            x,
            y,
            wheel,
            point,
        })
    }
}

#[allow(dead_code)]
impl SoftKeyboard {
    pub fn parse(body: &[u8]) -> Result<Self> {
        if body.len() < SOFT_KEYBOARD_LEN {
            bail!(
                "soft_keyboard body too short: {} < {}",
                body.len(),
                SOFT_KEYBOARD_LEN
            );
        }
        let mut button = [0i8; 10];
        for (i, b) in button.iter_mut().enumerate() {
            *b = body[2 + i] as i8;
        }
        Ok(SoftKeyboard {
            ctrl: body[0] as i8,
            resvel: body[1] as i8,
            button,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::kmbox_net::schema::{CMD_CONNECT, CMD_MOUSE_MOVE};

    #[test]
    fn parses_header_le() {
        // mac=0x01FBC068 rand=0xDEADBEEF indexpts=0x00000007 cmd=CMD_MOUSE_MOVE
        let mut buf = [0u8; 16];
        LittleEndian::write_u32(&mut buf[0..4], 0x01FBC068);
        LittleEndian::write_u32(&mut buf[4..8], 0xDEADBEEF);
        LittleEndian::write_u32(&mut buf[8..12], 0x00000007);
        LittleEndian::write_u32(&mut buf[12..16], CMD_MOUSE_MOVE);

        let h = Header::parse(&buf).unwrap();
        assert_eq!(h.mac, 0x01FBC068);
        assert_eq!(h.rand, 0xDEADBEEF);
        assert_eq!(h.indexpts, 7);
        assert_eq!(h.cmd, CMD_MOUSE_MOVE);
    }

    #[test]
    fn header_reply_increments_index_and_keeps_other_fields() {
        let h = Header {
            mac: 0x01FBC068,
            rand: 0xAABBCCDD,
            indexpts: 42,
            cmd: CMD_CONNECT,
        };
        let r = h.reply();
        assert_eq!(LittleEndian::read_u32(&r[0..4]), 0x01FBC068);
        assert_eq!(LittleEndian::read_u32(&r[4..8]), 0xAABBCCDD);
        assert_eq!(LittleEndian::read_u32(&r[8..12]), 43);
        assert_eq!(LittleEndian::read_u32(&r[12..16]), CMD_CONNECT);
    }

    #[test]
    fn parses_mouse_move_body() {
        // button=0, x=10, y=-3, wheel=0, point all zero
        let mut body = [0u8; SOFT_MOUSE_LEN];
        LittleEndian::write_i32(&mut body[0..4], 0);
        LittleEndian::write_i32(&mut body[4..8], 10);
        LittleEndian::write_i32(&mut body[8..12], -3);
        LittleEndian::write_i32(&mut body[12..16], 0);
        let m = SoftMouse::parse(&body).unwrap();
        assert_eq!(m.button, 0);
        assert_eq!(m.x, 10);
        assert_eq!(m.y, -3);
        assert_eq!(m.wheel, 0);
        assert_eq!(m.point, [0; 10]);
    }

    #[test]
    fn parses_automove_with_duration_and_points() {
        let mut body = [0u8; SOFT_MOUSE_LEN];
        LittleEndian::write_i32(&mut body[4..8], 300);
        LittleEndian::write_i32(&mut body[8..12], -50);
        LittleEndian::write_i32(&mut body[16..20], 120);
        LittleEndian::write_i32(&mut body[20..24], 10);
        LittleEndian::write_i32(&mut body[24..28], 20);
        LittleEndian::write_i32(&mut body[28..32], 30);
        LittleEndian::write_i32(&mut body[32..36], 40);
        let m = SoftMouse::parse(&body).unwrap();
        assert_eq!(m.x, 300);
        assert_eq!(m.y, -50);
        assert_eq!(m.point[0], 120);
        assert_eq!(m.point[1], 10);
        assert_eq!(m.point[2], 20);
        assert_eq!(m.point[3], 30);
        assert_eq!(m.point[4], 40);
    }
}
