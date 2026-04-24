require("related").setup({
	rules = {
		{
			exts = { ".cc", ".h" },
			prefix_strip = { "_test$", "_main$" },
			suffixes = {
				{ suffix = ".cc", type = "source" },
				{ suffix = ".h", type = "source" },
				{ suffix = "_test.cc", type = "test" },
				{ suffix = "_main.cc", type = "main" },
			},
		},
		{
			exts = { ".py" },
			prefix_strip = { "_test$", "_main$" },
			suffixes = {
				{ suffix = ".py", type = "source" },
				{ suffix = "_test.py", type = "test" },
				{ suffix = "_main.py", type = "main" },
			},
		},
	},
})
