import taichi as ti

@ti.program_test
def test_abs():
  ti.reset()
  x = ti.var(ti.f32)
  y = ti.var(ti.f32)

  N = 16
  @ti.layout
  def place():
    ti.root.dense(ti.i, N).place(x)
    ti.root.dense(ti.i, N).place(y)
    ti.root.lazy_grad()

  @ti.kernel
  def func():
    for i in range(N):
      x[i] = ti.abs(y[i])

  for i in range(N):
    y[i] = i - 10
    x.grad[i] = 1

  func()
  func.grad()

  def sgn(x):
    if x > 0:
      return 1
    if x < 0:
      return -1
    return 0

  for i in range(N):
    assert x[i] == abs(y[i])
    assert y.grad[i] == sgn(y[i])
