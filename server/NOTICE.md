# Notice

This folder is licensed under the GNU Affero General Public License v3.0 or later (see `LICENSE`).
The rest of the repository keeps its own licence; the game talks to this server only over HTTP.

## Credits

The wire format of the Turn 10 title services (HTTP POST with a `bin/xtsw` body, big-endian packed
records, the 12-byte reply header) and the idea of a self-hosted stand-in server are documented by,
and follow the approach of:

- forza-live, the self-hostable server of Forza Horizon 1: ReImagined (AGPL-3.0), https://github.com/FH1-ReImagined/forza-live
- FH2MP (GPL-3.0), https://github.com/Charelene/FH2MP

Code adapted from those projects is marked in the source with its origin.
