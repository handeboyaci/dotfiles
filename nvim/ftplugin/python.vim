set colorcolumn=89

let b:ale_fixers = ['black', 'isort']

let b:ale_python_isort_options = '--multi-line=3 --trailing-comma ' .
                               \ '--force-grid-wrap=0 --use-parentheses ' .
                               \ '--line-width=88'


let b:ale_linters = ['pyls']
let b:ale_python_pyls_config = {
      \ 'pyflakes':   {'enabled': v:false},
      \ 'pydocstyle': {'enabled': v:false},
      \ 'mypy':       {'enabled': v:true},
      \ 'flake8':     {'enabled': v:true}
      \}

nnoremap gd :ALEGoToDefinition<CR>
nnoremap gD :ALEGoToTypeDefinition<CR>
nnoremap K :ALEHover<CR>
nnoremap <Leader>R :ALEFindReferences<CR>


command! Black :py3 Black()

augroup Python
  autocmd!
  autocmd Python BufWritePre  * execute ':Black'
augroup END

call textobj#user#plugin('jupyter', {
      \    'select': {
      \      'select': 'ij',
      \      'pattern': '^#%%\_.\{-}\ze\(#%%\|\%$\)'
      \    },
      \    'move': {
      \      'pattern': '\(^#%%\|\%$\)',
      \      'move-n': '}',
      \      'move-p': '{',
      \    }
      \  })

python3 << endpython3
import time
import black
import vim


def Black():
    start = time.time()
    mode = black.FileMode.AUTO_DETECT
    buffer_str = "\n".join(vim.current.buffer) + "\n"

    try:
        new_buffer_str = black.format_file_contents(
            buffer_str, line_length=88, fast=True, mode=mode
        )
    except black.NothingChanged:
        print(f"Already well formatted, good job. (took {time.time() - start:.4f}s)")
    except Exception as exc:
        print(exc)
    else:
        vim.current.buffer[:] = new_buffer_str.split("\n")[:-1]
        print(f"Reformatted in {time.time() - start:.4f}s.")

endpython3
