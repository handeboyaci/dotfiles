require("related").setup({
	rules = {
		{
			exts = { ".cc", ".h" },
			prefix_strip = { "_test$", "_main$" },
			suffixes = { ".cc", ".h", "_test.cc", "_main.cc" },
		},
		{
			exts = { ".py" },
			prefix_strip = { "_test$", "_main$" },
			suffixes = { ".py", "_test.py", "_main.py" },
		},
	},
})
