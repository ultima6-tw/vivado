`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// async_fifo.v  (Verilog-2001 compatible)
// - Dual-clock FIFO with Gray-coded pointers and 2-FF synchronizers
// - DEPTH must be power-of-two
// -----------------------------------------------------------------------------
module async_fifo #(
    parameter integer WIDTH = 32,
    parameter integer DEPTH = 8   // power of 2: 2,4,8,16...
)(
    // Write (source / AXI GPIO clock domain)
    input  wire              wclk,
    input  wire              wrst_n,
    input  wire              w_en,
    input  wire [WIDTH-1:0]  w_data,
    output wire              w_full,

    // Read (dest / waveform_top clock domain)
    input  wire              rclk,
    input  wire              rrst_n,
    input  wire              r_en,
    output reg  [WIDTH-1:0]  r_data,
    output wire              r_empty
);
    // Address width (Vivado supports $clog2)
    localparam integer AW = $clog2(DEPTH);

    // Storage
    reg [WIDTH-1:0] mem [0:DEPTH-1];

    // Binary & Gray pointers (AW+1 bits: extra MSB for full/empty)
    reg [AW:0] wptr_bin, wptr_gray;
    reg [AW:0] rptr_bin, rptr_gray;

    // Cross-domain synchronized pointers
    reg [AW:0] rptr_gray_wclk_1, rptr_gray_wclk_2;  // read ptr into write domain
    reg [AW:0] wptr_gray_rclk_1, wptr_gray_rclk_2;  // write ptr into read domain

    // --- Write side FULL detection ---
    wire [AW:0] wptr_bin_next  = wptr_bin + {{AW{1'b0}}, 1'b1};
    wire [AW:0] wptr_gray_next = (wptr_bin_next >> 1) ^ wptr_bin_next;

    // FULL when next write gray equals read gray with MSBs inverted
    wire w_full_int =
        (wptr_gray_next[AW:AW-1] == ~rptr_gray_wclk_2[AW:AW-1]) &&
        (wptr_gray_next[AW-2:0]  ==  rptr_gray_wclk_2[AW-2:0]);
    assign w_full = w_full_int;

    // --- Read side EMPTY detection ---
    wire r_empty_int = (wptr_gray_rclk_2 == rptr_gray);
    assign r_empty   = r_empty_int;

    // =========================
    // Write clock domain (wclk)
    // =========================
    always @(posedge wclk or negedge wrst_n) begin
        if (!wrst_n) begin
            wptr_bin          <= {(AW+1){1'b0}};
            wptr_gray         <= {(AW+1){1'b0}};
            rptr_gray_wclk_1  <= {(AW+1){1'b0}};
            rptr_gray_wclk_2  <= {(AW+1){1'b0}};
        end else begin
            // sync read pointer gray into write domain
            rptr_gray_wclk_1 <= rptr_gray;
            rptr_gray_wclk_2 <= rptr_gray_wclk_1;

            // write when enabled and not full
            if (w_en && !w_full_int) begin
                mem[wptr_bin[AW-1:0]] <= w_data;
                wptr_bin  <= wptr_bin_next;
                wptr_gray <= wptr_gray_next;
            end
        end
    end

    // =========================
    // Read clock domain (rclk)
    // =========================
    always @(posedge rclk or negedge rrst_n) begin
        if (!rrst_n) begin
            rptr_bin          <= {(AW+1){1'b0}};
            rptr_gray         <= {(AW+1){1'b0}};
            wptr_gray_rclk_1  <= {(AW+1){1'b0}};
            wptr_gray_rclk_2  <= {(AW+1){1'b0}};
            r_data            <= {WIDTH{1'b0}};
        end else begin
            // sync write pointer gray into read domain
            wptr_gray_rclk_1 <= wptr_gray;
            wptr_gray_rclk_2 <= wptr_gray_rclk_1;

            // read when enabled and not empty
            if (r_en && !r_empty_int) begin
                r_data    <= mem[rptr_bin[AW-1:0]];
                rptr_bin  <= rptr_bin + {{AW{1'b0}}, 1'b1};
                rptr_gray <= ( (rptr_bin + {{AW{1'b0}}, 1'b1}) >> 1 )
                             ^ ( rptr_bin + {{AW{1'b0}}, 1'b1} );
            end
        end
    end
endmodule