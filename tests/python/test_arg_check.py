import taichi as ti

@ti.program_test
def test_argument_error():
  x = ti.var(ti.i32)
  
  @ti.layout
  def layout():
    ti.root.place(x)
  
  try:
    @ti.kernel
    def set_i32_notype(v):
      pass
  except ti.KernelDefError:
    pass
    
  try:
    @ti.kernel
    def set_i32_args(*args):
      pass
  except ti.KernelDefError:
    pass
  
  try:
    @ti.kernel
    def set_i32_kwargs(**kwargs):
      pass
  except ti.KernelDefError:
    pass
  
  @ti.kernel
  def set_i32(v: ti.i32):
    x[None] = v
    
  set_i32(123)
  assert x[None] == 123

