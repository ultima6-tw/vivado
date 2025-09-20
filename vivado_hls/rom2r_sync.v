`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// rom2r_sync.v
// - Dual-read synchronous ROM (generic)
// - Parameters: DATA_WIDTH, ADDR_WIDTH, ROM_DEPTH, INIT_FILE
// -----------------------------------------------------------------------------
module rom2r_sync #(
    parameter integer DATA_WIDTH = 32,
    parameter integer ADDR_WIDTH = 10,   // 2^10 = 1024 depth
    parameter integer ROM_DEPTH  = 1024,
    parameter        INIT_FILE   = "lut_q31_origin.mem"
)(
    input  wire                     clk,
    input  wire [ADDR_WIDTH-1:0]    addr_a,
    output reg  [DATA_WIDTH-1:0]    dout_a,
    input  wire [ADDR_WIDTH-1:0]    addr_b,
    output reg  [DATA_WIDTH-1:0]    dout_b
);

    // ROM storage
    reg [DATA_WIDTH-1:0] rom [0:ROM_DEPTH-1];

    // Optional init
    initial begin
        if (INIT_FILE != "") begin
            $readmemh(INIT_FILE, rom);
        end
    end

    // Synchronous read
    always @(posedge clk) begin
        dout_a <= rom[addr_a];
        dout_b <= rom[addr_b];
    end

endmodule