`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// lut_rom2r.v  (wrapper to match your existing interface)
// - Drop-in compatible with previous usage:
//     Parameters: LUT_DEPTH, INIT_FILE (optional DATA_WIDTH override)
//     Ports     : clk, addr_a, dout_a, addr_b, dout_b
// - Internally instantiates rom2r_sync with ADDR_WIDTH = clog2(LUT_DEPTH)
// -----------------------------------------------------------------------------
module lut_rom2r #(
    parameter integer LUT_DEPTH  = 4096,
    parameter        INIT_FILE   = "lut_q31_origin.mem",
    parameter integer DATA_WIDTH = 32
)(
    input  wire                          clk,
    input  wire [ADDR_W-1:0]             addr_a,
    output wire [DATA_WIDTH-1:0]         dout_a,
    input  wire [ADDR_W-1:0]             addr_b,
    output wire [DATA_WIDTH-1:0]         dout_b
);
    // Compute address width from LUT_DEPTH (Verilog-2001-safe CLOG2)
    function integer CLOG2;
        input integer value;
        integer v, i;
    begin
        v = (value > 0) ? value - 1 : 0;
        i = 0;
        while (v > 0) begin
            v = v >> 1;
            i = i + 1;
        end
        CLOG2 = i;
    end
    endfunction

    localparam integer ADDR_W = CLOG2(LUT_DEPTH);

    // Tie-off localparam to port param
    // Note: some tools require a localparam visible before port decl; hence two-step
    // Re-declare ADDR_W for port use via localparam above
    // Synthesis treats this as wiring, not extra logic.

    // Instantiate the generic ROM
    rom2r_sync #(
        .DATA_WIDTH (DATA_WIDTH),
        .ADDR_WIDTH (ADDR_W),
        .ROM_DEPTH  (LUT_DEPTH),
        .INIT_FILE  (INIT_FILE)
    ) u_rom (
        .clk    (clk),
        .addr_a (addr_a),
        .dout_a (dout_a),
        .addr_b (addr_b),
        .dout_b (dout_b)
    );

endmodule