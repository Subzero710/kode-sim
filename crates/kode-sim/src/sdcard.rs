//! The microSD card in the simulated Kode Dot's slot: a card image on the Mac.
//!
//! kemu's card model answers the SD protocol; its blocks are here, in a raw
//! image file, read and written as the firmware asks over UART1 (SD_READ,
//! SD_WRITE, SD_DATA). The image is a card as cards come formatted - an MBR
//! and one FAT32 partition from sector 2048 - so the Mac can open it too:
//! `diskutil image attach card.img` while the simulator is not running.

use std::fs::{File, OpenOptions};
use std::io;
use std::os::fd::AsRawFd;
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

pub const BLOCK: usize = 512;
pub const DEFAULT_MB: u64 = 1024;
pub const LABEL: &str = "KODEDOT";
/// Where a card's one partition starts, as SD cards come formatted.
pub const FIRST_LBA: u64 = 2048;

/// The card `kode-sim run` uses unless told otherwise.
pub fn default_path() -> PathBuf {
    crate::emu::cache_dir().join("sd").join("card.img")
}

/// A blank card: a sparse file of `size_mb`, an MBR, and one FAT32 volume
/// from sector 2048 on. mtools' mformat writes the volume straight into the
/// file - no loop device, no root, the same on the Mac and in a container -
/// and diskutil is the Mac's fallback when mtools is not installed.
pub fn make_card(path: &Path, size_mb: u64, label: &str) -> Result<(), String> {
    let fail = |e: String| {
        let _ = std::fs::remove_file(path);
        Err(e)
    };
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("could not make {}: {e}", dir.display()))?;
    }
    let sectors = size_mb * 2048;
    let made = (|| -> io::Result<()> {
        let f = File::create(path)?;
        f.set_len(sectors * BLOCK as u64)?; // sparse: only what is written takes room
        let mut mbr = [0u8; BLOCK];
        // One partition entry: not bootable, CHS "past 8 GB", FAT32 (LBA).
        mbr[446..450].copy_from_slice(&[0x00, 0xfe, 0xff, 0xff]);
        mbr[450..454].copy_from_slice(&[0x0C, 0xfe, 0xff, 0xff]);
        mbr[454..458].copy_from_slice(&(FIRST_LBA as u32).to_le_bytes());
        mbr[458..462].copy_from_slice(&((sectors - FIRST_LBA) as u32).to_le_bytes());
        mbr[510..512].copy_from_slice(&[0x55, 0xaa]);
        f.write_all_at(&mbr, 0)
    })();
    if let Err(e) = made {
        return fail(format!("could not make {}: {e}", path.display()));
    }
    if crate::emu::which("mformat") {
        // FAT32 wants 65525 clusters at least: 4 KB ones from 512 MB up, 512 B below.
        let per_cluster = if sectors >= 1024 * 2048 / 2 { "8" } else { "1" };
        let image = format!("{}@@{}", path.display(), FIRST_LBA * BLOCK as u64);
        let total = (sectors - FIRST_LBA).to_string();
        let out = Command::new("mformat")
            .args(["-i", &image, "-F", "-T", &total, "-c", per_cluster, "-v", label, "::"])
            .output();
        return match out {
            Ok(o) if o.status.success() => Ok(()),
            Ok(o) => fail(format!("could not format {}: {}", path.display(), said(&o))),
            Err(e) => fail(format!("could not format {}: {e}", path.display())),
        };
    }
    if crate::emu::which("diskutil") {
        return make_card_diskutil(path, label).or_else(fail);
    }
    fail("no mformat to format the card with: install mtools".into())
}

/// What a tool said on failing: its stderr, or else its stdout.
fn said(o: &Output) -> String {
    let err = String::from_utf8_lossy(&o.stderr).trim().to_string();
    if err.is_empty() {
        String::from_utf8_lossy(&o.stdout).trim().to_string()
    } else {
        err
    }
}

fn make_card_diskutil(path: &Path, label: &str) -> Result<(), String> {
    let attach = Command::new("diskutil")
        .args(["image", "attach", "--noMount"])
        .arg(path)
        .output()
        .map_err(|e| format!("could not attach {}: {e}", path.display()))?;
    let out = String::from_utf8_lossy(&attach.stdout).to_string();
    let dev = out.split_whitespace().next().unwrap_or("").to_string();
    if !attach.status.success() || !dev.starts_with("/dev/disk") {
        return Err(format!("could not attach {}: {}", path.display(), said(&attach)));
    }
    let fmt = Command::new("diskutil").args(["partitionDisk", &dev, "MBR", "MS-DOS FAT32", label, "100%"]).output();
    let _ = Command::new("diskutil").args(["eject", &dev]).output();
    match fmt {
        Ok(o) if o.status.success() => Ok(()),
        Ok(o) => Err(format!("could not format {}: {}", path.display(), said(&o))),
        Err(e) => Err(format!("could not format {}: {e}", path.display())),
    }
}

/// A card image, its blocks read and written in place.
pub struct Card {
    file: File,
    pub sectors: u64,
}

impl Card {
    /// The image at `path`, for this simulator alone: two on one image
    /// would each write its FAT over the other's. The lock (flock, an
    /// advisory one) goes with the card.
    pub fn open(path: &Path) -> io::Result<Card> {
        let file = OpenOptions::new().read(true).write(true).open(path)?;
        if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
            let e = io::Error::last_os_error();
            if e.kind() == io::ErrorKind::WouldBlock {
                return Err(io::Error::new(
                    io::ErrorKind::WouldBlock,
                    format!(
                        "{} is the card of another simulator already running: give this one its own with --sd OTHER.img",
                        path.display()
                    ),
                ));
            }
            return Err(e);
        }
        let sectors = file.metadata()?.len() / BLOCK as u64;
        Ok(Card { file, sectors })
    }

    /// `count` blocks from `lba`, or nothing if that runs past the card's end.
    pub fn read(&self, lba: u64, count: u64) -> Vec<u8> {
        if lba + count > self.sectors {
            return vec![];
        }
        let mut v = vec![0; count as usize * BLOCK];
        match self.file.read_exact_at(&mut v, lba * BLOCK as u64) {
            Ok(()) => v,
            Err(_) => vec![],
        }
    }

    /// Whole blocks from `lba`. Blocks that are not whole or do not fit are
    /// an InvalidInput error (the firmware's); any other, the image's.
    pub fn write(&self, lba: u64, data: &[u8]) -> io::Result<()> {
        let bad = |why: String| Err(io::Error::new(io::ErrorKind::InvalidInput, why));
        if !data.len().is_multiple_of(BLOCK) {
            return bad(format!("{} bytes at block {lba} are not whole blocks", data.len()));
        }
        let n = (data.len() / BLOCK) as u64;
        if lba + n > self.sectors {
            return bad(format!("{n} blocks at {lba} run past the card's end ({} blocks)", self.sectors));
        }
        self.file.write_all_at(data, lba * BLOCK as u64)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("kode-sim-test-{}-{name}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("card.img")
    }

    fn have_formatter() -> bool {
        crate::emu::which("mformat") || crate::emu::which("diskutil")
    }

    #[test]
    fn a_new_card_is_mbr_and_fat32_from_sector_2048() {
        if !have_formatter() {
            eprintln!("no mtools (or diskutil) to format a card with");
            return;
        }
        let img = tmp("fat32");
        make_card(&img, 64, LABEL).unwrap();
        assert_eq!(std::fs::metadata(&img).unwrap().len(), 64 * 1024 * 1024);
        let d = std::fs::read(&img).unwrap();
        assert_eq!(&d[510..512], b"\x55\xaa"); // MBR
        let ptype = d[446 + 4];
        let lba = u32::from_le_bytes(d[446 + 8..446 + 12].try_into().unwrap()) as usize;
        assert!(ptype == 0x0B || ptype == 0x0C, "{ptype:#x}");
        if crate::emu::which("mformat") {
            assert_eq!(lba, 2048); // FAT32, where cards have it
        } else {
            // diskutil puts it where its macOS does (sector 63 on recent ones);
            // the firmware finds it in the MBR wherever it is.
            assert!(lba >= 63, "{lba}");
        }
        let boot = &d[lba * 512..(lba + 1) * 512];
        assert_eq!(&boot[82..90], b"FAT32   ");
        assert_eq!(&boot[71..82], b"KODEDOT    ");
        std::fs::remove_dir_all(img.parent().unwrap()).unwrap();
    }

    #[test]
    fn a_write_the_image_refuses_is_an_error_of_its_own() {
        let img = tmp("ro");
        File::create(&img).unwrap().set_len(1024 * 1024).unwrap();
        let file = File::open(&img).unwrap(); // read-only: every write fails
        let card = Card { sectors: 2048, file };
        let e = card.write(0, &[0; 512]).unwrap_err();
        assert_ne!(e.kind(), io::ErrorKind::InvalidInput, "{e}");
        std::fs::remove_dir_all(img.parent().unwrap()).unwrap();
    }

    #[test]
    fn a_card_is_in_one_simulator_at_a_time() {
        let img = tmp("lock");
        File::create(&img).unwrap().set_len(1024 * 1024).unwrap();
        let first = Card::open(&img).unwrap();
        let e = Card::open(&img).err().expect("a second simulator opened the same card");
        let said = e.to_string();
        assert!(said.contains(&img.display().to_string()) && said.contains("--sd"), "{said}");
        drop(first);
        // Free again once the first lets it go - after a moment at most: a
        // child another test forks holds every descriptor until its exec.
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
        while let Err(e) = Card::open(&img) {
            assert!(std::time::Instant::now() < deadline, "still taken: {e}");
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
        std::fs::remove_dir_all(img.parent().unwrap()).unwrap();
    }

    #[test]
    fn a_card_reads_and_writes_blocks_in_place() {
        let img = tmp("io");
        File::create(&img).unwrap().set_len(64 * 1024 * 1024).unwrap();
        let block: Vec<u8> = (0..=255u8).collect();
        let two = block.repeat(4);
        {
            let card = Card::open(&img).unwrap();
            assert_eq!(card.sectors, 64 * 2048);
            card.write(5000, &two).unwrap();
            assert_eq!(card.read(5000, 2), two);
            assert!(card.read(card.sectors, 1).is_empty()); // past the end: nothing
            let past = card.write(card.sectors, &[0; 512]).unwrap_err();
            assert_eq!(past.kind(), io::ErrorKind::InvalidInput);
            assert!(past.to_string().contains("past the card's end"), "{past}");
            let torn = card.write(0, &[0; 100]).unwrap_err(); // not a whole block
            assert_eq!(torn.kind(), io::ErrorKind::InvalidInput);
        }
        let d = std::fs::read(&img).unwrap();
        assert_eq!(&d[5000 * 512..5001 * 512], &block.repeat(2)[..]);
        std::fs::remove_dir_all(img.parent().unwrap()).unwrap();
    }
}
