nnoremap <buffer> <silent> gf f"h"oyiwF"gf:call search("name = \"<C-R><C-R>o\"")<CR>
" Trim // from the beginning; if there are no slices left re-open this file as a
" hack, otherwise open the BUILD file in the v:fname path.
set includeexpr={x->count(x,\"/\")==0?expand(\"%\"):x.\"/BUILD\"}(trim(v:fname,\"/\"))

