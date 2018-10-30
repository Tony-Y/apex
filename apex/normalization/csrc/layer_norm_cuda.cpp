#include <torch/torch.h>
#include <vector>
#include <cassert>

namespace {
void compute_n1_n2(
    at::Tensor input,
    at::IntList normalized_shape,
    int& n1,
    int& n2)
{
    int idiff = input.ndimension() - normalized_shape.size();
    n2 = 1;
    for (int i = 0;  i < (int)normalized_shape.size();  ++i) {
	    assert( input.sizes()[i+idiff] == normalized_shape[i] );
	    n2 *= normalized_shape[i];
    }
    n1 = 1;
    for (int i = 0;  i < idiff;  ++i) {
	    n1 *= input.sizes()[i];
    }
}

void check_args(
    at::IntList normalized_shape,
    at::Tensor gamma,
    at::Tensor beta
    )
{
    if (gamma.defined() && !gamma.sizes().equals(normalized_shape)) {
      std::stringstream ss;
      ss << "Expected gamma to be of same shape as normalized_shape, but got "
         << "gamma of shape " << gamma.sizes() << " and normalized_shape="
         << normalized_shape;
      throw std::runtime_error(ss.str());
    }

    if (beta.defined() && !beta.sizes().equals(normalized_shape)) {
      std::stringstream ss;
      ss << "Expected beta to be of same shape as normalized_shape, but got "
         << "beta of shape " << beta.sizes() << " and normalized_shape="
         << normalized_shape;
      throw std::runtime_error(ss.str());
    }
}

void check_args(
    at::Tensor input,
    at::IntList normalized_shape,
    int& n1,
    int& n2
    )
{
    int64_t normalized_ndim = normalized_shape.size();

    if (normalized_ndim < 1) {
      std::stringstream ss;
      ss << "Expected normalized_shape to be at least 1-dimensional, i.e., "
         << "containing at least one element, but got normalized_shape="
         << normalized_shape;
      throw std::runtime_error(ss.str());
    }

    auto input_shape = input.sizes();
    auto input_ndim = input.dim();

    if (input_ndim < normalized_ndim ||
        !input_shape.slice(input_ndim - normalized_ndim).equals(normalized_shape)) {
      std::stringstream ss;
      ss << "Given normalized_shape=" << normalized_shape
         << ", expected input with shape [*";
      for (auto size : normalized_shape) {
        ss << ", " << size;
      }
      ss << "], but got input of size" << input_shape;
      throw std::runtime_error(ss.str());
    }

    compute_n1_n2(input,normalized_shape,n1,n2);
}


void check_args(
    at::Tensor input,
    at::IntList normalized_shape,
    at::Tensor gamma,
    at::Tensor beta,
    int& n1,
    int& n2
    )
{
    check_args(input,normalized_shape,n1,n2);
    check_args(normalized_shape,gamma,beta);
}

template<typename T> 
void allocate_layer_norm_output_tensors(
    at::Tensor input,
    const T* input_data,
    int n1,
    at::Tensor& output,
    at::Tensor& mean,
    at::Tensor& invvar
    )
{
  output = at::empty_like(input);
  mean = at::empty({n1}, input.options().dtype(at::kFloat));
  invvar = at::empty_like(mean);
}
template<> 
void allocate_layer_norm_output_tensors(
    at::Tensor input,
    const int64_t* input_data,
    int n1,
    at::Tensor& output,
    at::Tensor& mean,
    at::Tensor& invvar
    )
{
  output = at::empty_like(input);
  mean = at::empty({n1}, input.options().dtype(at::kDouble));
  invvar = at::empty_like(mean);
}
template<> 
void allocate_layer_norm_output_tensors(
    at::Tensor input,
    const double* input_data,
    int n1,
    at::Tensor& output,
    at::Tensor& mean,
    at::Tensor& invvar
    )
{
  output = at::empty_like(input);
  mean = at::empty({n1}, input.options().dtype(at::kDouble));
  invvar = at::empty_like(mean);
}
}

void cuda_layer_norm(
    at::Tensor* output,
    at::Tensor* mean,
    at::Tensor* invvar,
    at::Tensor* input,
    int n1,
    int n2,
    at::IntList normalized_shape,
    at::Tensor* gamma,
    at::Tensor* beta,
    double epsilon);

#define CHECK_CUDA(x) AT_ASSERTM(x.type().is_cuda(), #x " must be a CUDA tensor")
#define CHECK_CONTIGUOUS(x) AT_ASSERTM(x.is_contiguous(), #x " must be contiguous")
#define CHECK_INPUT(x) CHECK_CUDA(x); CHECK_CONTIGUOUS(x)

std::vector<at::Tensor> layer_norm(
    at::Tensor input,
    at::IntList normalized_shape,
    double epsilon) {
  CHECK_INPUT(input);
  int n1,n2;
  check_args(input,normalized_shape,n1,n2);
  at::Tensor output;
  at::Tensor mean;
  at::Tensor invvar;
  AT_DISPATCH_ALL_TYPES_AND_HALF(input.type(), "allocate_layer_norm_output_tensors", ([&] {
        allocate_layer_norm_output_tensors(
            input,input.data<scalar_t>(),n1,
            output,mean,invvar);
      }));
  cuda_layer_norm(&output,&mean,&invvar,&input,n1,n2,
      normalized_shape,NULL,NULL,epsilon);
  return {output, mean, invvar};
}
std::vector<at::Tensor> layer_norm_affine(
    at::Tensor input,
    at::IntList normalized_shape,
    at::Tensor gamma,
    at::Tensor beta,
    double epsilon) {
  CHECK_INPUT(input);
  CHECK_INPUT(gamma);
  CHECK_INPUT(beta);
  int n1,n2;
  check_args(input,normalized_shape,gamma,beta,n1,n2);
  at::Tensor output;
  at::Tensor mean;
  at::Tensor invvar;
  AT_DISPATCH_ALL_TYPES_AND_HALF(input.type(), "allocate_layer_norm_output_tensors", ([&] {
        allocate_layer_norm_output_tensors(
            input,input.data<scalar_t>(),n1,
            output,mean,invvar);
      }));
  cuda_layer_norm(&output,&mean,&invvar,&input,n1,n2,
      normalized_shape,&gamma,&beta,epsilon);
  return {output, mean, invvar};
}

void cuda_layer_norm_gradient(
    at::Tensor* dout,
    at::Tensor* mean,
    at::Tensor* invvar,
    at::Tensor* input,
    int n1,
    int n2,
    at::IntList normalized_shape,
    at::Tensor* gamma,
    at::Tensor* beta,
    double epsilon,
    at::Tensor* grad_input,
    at::Tensor* grad_gamma,
    at::Tensor* grad_beta
    );

at::Tensor layer_norm_gradient(
    at::Tensor dout,
    at::Tensor mean,
    at::Tensor invvar,
    at::Tensor input,
    at::IntList normalized_shape,
    double epsilon) {
  CHECK_INPUT(dout);
  CHECK_INPUT(mean);
  CHECK_INPUT(invvar);
  CHECK_INPUT(input);
  int n1,n2;
  check_args(input,normalized_shape,n1,n2);
  at::Tensor grad_input = at::empty_like(input);
  cuda_layer_norm_gradient(&dout,&mean,&invvar,&input,n1,n2,
      normalized_shape,NULL,NULL,epsilon,
      &grad_input,NULL,NULL);
  return grad_input;
}
std::vector<at::Tensor> layer_norm_gradient_affine(
    at::Tensor dout,
    at::Tensor mean,
    at::Tensor invvar,
    at::Tensor input,
    at::IntList normalized_shape,
    at::Tensor gamma,
    at::Tensor beta,
    double epsilon) {
  CHECK_INPUT(dout);
  CHECK_INPUT(mean);
  CHECK_INPUT(invvar);
  CHECK_INPUT(input);
  CHECK_INPUT(gamma);
  CHECK_INPUT(beta);
  int n1,n2;
  check_args(input,normalized_shape,gamma,beta,n1,n2);
  at::Tensor grad_input = at::empty_like(input);
  at::Tensor grad_gamma = at::empty_like(gamma);
  at::Tensor grad_beta = at::empty_like(beta);
  cuda_layer_norm_gradient(&dout,&mean,&invvar,&input,n1,n2,
      normalized_shape,&gamma,&beta,epsilon,
      &grad_input,&grad_gamma,&grad_beta);
  return {grad_input, grad_gamma, grad_beta};
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("forward_affine", &layer_norm_affine, "LayerNorm forward (CUDA)");
  m.def("forward", &layer_norm, "LayerNorm forward (CUDA)");
  m.def("backward_affine", &layer_norm_gradient_affine, "LayerNorm backward (CUDA)");
  m.def("backward", &layer_norm_gradient, "LayerNorm backward (CUDA)");
}
