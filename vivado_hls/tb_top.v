`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 2025/06/23 19:13:44
// Design Name: 
// Module Name: tb_top
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////


module tb_top;

    reg sys_clock;
    
    // Clock generator (8ns period = 125MHz)
    initial begin
        sys_clock = 0;
        forever #4 sys_clock = ~sys_clock;
    end

    // DUT instantiation
    design_1_wrapper dut (
        .sys_clock(sys_clock)
    );

endmodule
