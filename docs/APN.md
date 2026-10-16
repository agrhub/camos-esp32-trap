# 4G APN auto-detect

With the APN setting empty (the default), the camera reads the SIM's IMSI (`AT+CIMI`), takes MCC + MNC (the first 5
digits) and picks the APN from the table in `firmware/main/apn.c`. An APN typed in the portal ("Advanced settings > 4G")
or the console (`set apn ...`) always wins. A network not in the table gets an empty APN: on LTE the network assigns the
default bearer at attach and that usually works; if data does not come up, set the APN by hand.

| MCC-MNC | Operator | APN | Status |
|---|---|---|---|
| 452-01 | MobiFone | `m-wap` | unverified |
| 452-02 | Vinaphone | `m3-world` | unverified |
| 452-04 | Viettel | `v-internet` | unverified |
| 452-05 | Vietnamobile | `internet` | unverified |
| 452-07 | Gmobile | `internet` | unverified |

"Unverified" means no page on the operator's own site was found that states the value (search on 16 Oct 2026). The
values agree across several third-party guides:
- Viettel `v-internet`, user and password empty: Mytour (mytour.vn, "set up 3G/4G network for your Android phone"),
  CellphoneS Sforum (cellphones.com.vn/sforum/cau-hinh-4g-viettel).
- Vinaphone `m3-world` (some guides add user / password `mms`): CellphoneS Sforum (cellphones.com.vn/sforum/apn-vinaphone-la-gi),
  FPT Shop (fptshop.com.vn, a 3G setup guide for Vinaphone), Droam wiki (wiki.droam.com/Vietnam).
- MobiFone `m-wap`, user and password empty per Sforum's reading of MobiFone's documentation
  (cellphones.com.vn/sforum/apn-mobifone); older guides add `mms` / `mms`.
- Vietnamobile `internet`, user and password empty: CellphoneS Sforum, Mytour, config-internet.com.
- Gmobile: no source checked; kept because `internet` is the common default.

Not in the table: iTel, Wintel (Reddi) and other MVNOs. Their MNC was not confirmed here, so the firmware does not guess.
The A7680C with an empty APN on LTE is expected to use the network default (to be checked on hardware).

PAP / CHAP user names: `esp_modem` sets only the APN. If Vinaphone or MobiFone need `mms` / `mms`, that is an open item
(an `AT+CGAUTH` before PPP); record what the SIM on the bench needs.

The choice is logged at every 4G start:
`I (...) modem: APN auto: 45204 Viettel -> "v-internet" (unverified, docs/APN.md)`.
