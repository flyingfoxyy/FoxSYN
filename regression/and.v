module top(a, b, c, o);
	input a, b, c;
	output o;

	assign o = a & b & c;
endmodule
