" Tmux Navigator.
let g:tmux_navigator_disable_when_zoomed = 1

tnoremap <silent> <C-h> <C-\><C-n>:TmuxNavigateLeft<CR>
tnoremap <silent> <C-j> <C-\><C-n>:TmuxNavigateDown<CR>
tnoremap <silent> <C-k> <C-\><C-n>:TmuxNavigateUp<CR>
tnoremap <silent> <C-l> <C-\><C-n>:TmuxNavigateRight<CR>
tnoremap <silent> <C-w> <C-\><C-n><C-w>
tnoremap <silent> <C-x> <C-\><C-n><C-w>c

" Surround.
let g:surround_no_insert_mappings = 1

" Undotree
nnoremap <silent> coz :UndotreeToggle<CR>

let g:signify_skip_filename_pattern = ['\.pipertmp.*']
