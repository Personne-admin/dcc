/// in order to start using zig build with dcc, add the dcc sdk with the following command.
/// zig fetch --save git+https://github.com/Personne-admin/dcc.git
const std = @import("std");

pub const sdk = @import("zig/dcc_sdk.zig");

pub fn build(b: *std.Build) void {
    _ = b.addModule("dcc", .{
        .root_source_file = b.path("zig/dcc_sdk.zig"),
    });
}
