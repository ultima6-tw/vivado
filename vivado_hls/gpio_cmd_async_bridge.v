`timescale 1ns/1ps
// -----------------------------------------------------------------------------
// gpio_cmd_async_bridge.v
// - Bridges gpio_wen/gpio_wdata from AXI GPIO clock to waveform_top clock
// - Source (PS/AXI GPIO): one-cycle gpio_wen pushes 32-bit command into async FIFO
// - Dest (PL/data clock): pops when available; emits one-cycle dst_wen without glitches
// -----------------------------------------------------------------------------
module gpio_cmd_async_bridge #(
    parameter integer WIDTH = 32,
    parameter integer DEPTH = 32
)(
    // Source domain (AXI GPIO clock)
    input  wire              src_clk,
    input  wire              src_rst_n,
    input  wire              gpio_wen_src,          // 1-cycle pulse in src_clk
    input  wire [WIDTH-1:0]  gpio_wdata_src,        // command word
    output wire              src_full,              // optional feedback to SW

    // Destination domain (waveform_top clock)
    input  wire              dst_clk,
    input  wire              dst_rst_n,
    output reg               gpio_wen_dst,          // 1-cycle pulse in dst_clk
    output reg  [WIDTH-1:0]  gpio_wdata_dst,        // aligned with gpio_wen_dst
    output wire              dst_empty              // for debug/monitor
);
    // --------------------
    // Async FIFO instance
    // --------------------
    wire              w_full;
    wire              r_empty;
    wire [WIDTH-1:0]  fifo_rdata;

    async_fifo #(
        .WIDTH (WIDTH),
        .DEPTH (DEPTH)
    ) u_af (
        .wclk   (src_clk),
        .wrst_n (src_rst_n),
        .w_en   (gpio_wen_src),
        .w_data (gpio_wdata_src),
        .w_full (w_full),

        .rclk   (dst_clk),
        .rrst_n (dst_rst_n),
        .r_en   (r_en),            // pop only when armed & not empty (generated below)
        .r_data (fifo_rdata),
        .r_empty(r_empty)
    );

    assign src_full  = w_full;
    assign dst_empty = r_empty;

    // ---------------------------------------------
    // Generate a clean one-cycle pop strobe (r_en)
    // ---------------------------------------------
    // Arm after reset to let CDC synchronizers settle (2-cycle shift register)
    reg [1:0] arm_sr;
    wire armed = arm_sr[1];

    always @(posedge dst_clk or negedge dst_rst_n) begin
        if (!dst_rst_n)
            arm_sr <= 2'b00;
        else
            arm_sr <= {arm_sr[0], 1'b1};
    end

    // Pop when armed and FIFO not empty; this is a single-cycle strobe
    reg r_en;
    always @(posedge dst_clk or negedge dst_rst_n) begin
        if (!dst_rst_n)
            r_en <= 1'b0;
        else
            r_en <= armed & ~r_empty;
    end

    // ---------------------------------------------
    // Output pulse and data (registered)
    // ---------------------------------------------
    always @(posedge dst_clk or negedge dst_rst_n) begin
        if (!dst_rst_n) begin
            gpio_wen_dst   <= 1'b0;
            gpio_wdata_dst <= {WIDTH{1'b0}};
        end else begin
            gpio_wen_dst <= r_en;           // exactly 1-cycle when we pop
            if (r_en)
                gpio_wdata_dst <= fifo_rdata;
        end
    end
endmodule