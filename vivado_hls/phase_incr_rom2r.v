// -----------------------------------------------------------------------------
// phase_incr_rom2r.v
// - Dual-read-port synchronous ROM for phase increment table
// - WIDTH=PHASE_W (e.g., 32), DEPTH=2^IDX_W by default
// -----------------------------------------------------------------------------
`timescale 1ns/1ps
module phase_incr_rom2r #(
    parameter integer PHASE_W   = 32,
    parameter integer IDX_W     = 10,                       // index width
    parameter integer ROM_DEPTH = (1 << IDX_W),             // default depth
    parameter         INIT_FILE = "phase_incr_origin.mem"          // default file
)(
    input  wire                          clk,
    input  wire [$clog2(ROM_DEPTH)-1:0]  addr_a,            // index A
    output wire [PHASE_W-1:0]            dout_a,            // dphi A
    input  wire [$clog2(ROM_DEPTH)-1:0]  addr_b,            // index B
    output wire [PHASE_W-1:0]            dout_b             // dphi B
);
    rom2r_sync #(
        .WIDTH    (PHASE_W),
        .DEPTH    (ROM_DEPTH),
        .INIT_FILE(INIT_FILE)
    ) u_rom (
        .clk   (clk),
        .addr_a(addr_a),
        .dout_a(dout_a),
        .addr_b(addr_b),
        .dout_b(dout_b)
    );
endmodule