Features
========

Device-Agnostic
***************

Easy-to-use Python API
----------------------

Intel® Extension for PyTorch\* provides simple frontend Python APIs and utilities to get performance optimizations such as operator optimization.

Check the `API Documentation <api_doc.html>`_ for API functions description and `Examples <examples.md>`_ for usage guidance.

Channels Last
-------------

Compared with the default NCHW memory format, using channels_last (NHWC) memory format can further accelerate convolutional neural networks. In Intel® Extension for PyTorch\*, NHWC memory format has been enabled for most key CPU and GPU operators. More detailed information is available at `Channels Last <features/nhwc.md>`_.

Intel® Extension for PyTorch* automatically converts a model to channels last memory format when users optimize the model with ``ipex.optimize(model)``. With this feature, users do not need to manually apply ``model=model.to(memory_format=torch.channels_last)`` anymore. However, models running on Intel® Data Center GPU Flex Series will choose oneDNN layout, so users still need to manually convert the model and data to channels last format. More detailed information is available at `Auto Channels Last <features/auto_channels_last.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/nhwc
   features/auto_channels_last


Auto Mixed Precision (AMP)
--------------------------

Benefiting from less memory usage and computation, low precision data types typically speed up both training and inference workloads. Furthermore, accelerated by Intel® native hardware instructions, including Intel® Deep Learning Boost (Intel® DL Boost) on the 3rd Generation Xeon® Scalable Processors (aka Cooper Lake), as well as the Intel® Advanced Matrix Extensions (Intel® AMX) instruction set on the 4th next generation of Intel® Xeon® Scalable Processors (aka Sapphire Rapids), low precision data type, bfloat 16 and float16, provide further boosted performance. We recommend to use AMP for accelerating convolutional and matmul based neural networks.

The support of Auto Mixed Precision (AMP) with `BFloat16 on CPU <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-deep-learning-boost-new-instruction-bfloat16.html>`_ and BFloat16 optimization of operators has been enabled in Intel® Extension for PyTorch\*, and partially upstreamed to PyTorch master branch. These optimizations will be landed in PyTorch master through PRs that are being submitted and reviewed. On GPU side, support of BFloat16 and Float16 are both available in Intel® Extension for PyTorch\*. BFloat16 is the default low precision floating data type when AMP is enabled.

Detailed information of AMP for GPU and CPU are available at `Auto Mixed Precision (AMP) on GPU <features/amp_gpu.md>`_ and `Auto Mixed Precision (AMP) on CPU <features/amp_cpu.md>`_ respectively.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/amp_cpu
   features/amp_gpu


Quantization
------------

Intel® Extension for PyTorch* provides built-in INT8 quantization recipes to deliver good statistical accuracy for most popular DL workloads including CNN, NLP and recommendation models on CPU side. On top of that, if users would like to tune for a higher accuracy than what the default recipe provides, a recipe tuning API powered by Intel® Neural Compressor is provided for users to try.

Check more detailed information for `INT8 Quantization [CPU] <features/int8_overview.md>`_ and `INT8 recipe tuning API guide (Experimental, *NEW feature in 1.13.0* on CPU) <features/int8_recipe_tuning_api.md>`_ on CPU side.

Check more detailed information for `INT8 Quantization [XPU] <features/int8_overview_xpu.md>`_. 

On Intel® GPUs, Intel® Extension for PyTorch* also provides INT4 and FP8 Quantization.  Check more detailed information for `FP8 Quantization <./features/float8.md>`_ and `INT4 Quantization <./features/int4.md>`_ 

.. toctree::
   :hidden:
   :maxdepth: 1

   features/int8_overview
   features/int8_recipe_tuning_api
   features/int8_overview_xpu
   features/int4
   features/float8


Distributed Training
--------------------

To meet demands of large scale model training over multiple devices, distributed training on Intel® GPUs and CPUs are supported. Two alternative methodologies are available. Users can choose either to use PyTorch native distributed training module, `Distributed Data Parallel (DDP) <https://pytorch.org/docs/stable/notes/ddp.html>`_, with `Intel® oneAPI Collective Communications Library (oneCCL) <https://www.intel.com/content/www/us/en/developer/tools/oneapi/oneccl.html>`_ support via `Intel® oneCCL Bindings for PyTorch (formerly known as torch_ccl) <https://github.com/intel/torch-ccl>`_ or use Horovod with `Intel® oneAPI Collective Communications Library (oneCCL) <https://www.intel.com/content/www/us/en/developer/tools/oneapi/oneccl.html>`_ support (Experimental).

For more detailed information, check `DDP <features/DDP.md>`_ and `Horovod (Experimental) <features/horovod.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/DDP
   features/horovod


GPU-Specific
************

DLPack Solution
---------------

DLPack defines a stable in-memory data structure for sharing tensors among frameworks. It enables sharing of tensor data without copying when interoparating with other libraries. Intel® Extension for PyTorch* extends DLPack support in PyTorch* for XPU device particularly.

For more detailed information, check `DLPack Solution <features/DLPack.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/DLPack

DPC++ Extension
---------------

Intel® Extension for PyTorch\* provides C++ APIs to get SYCL queue and configure floating-point math mode.

Check the `API Documentation`_ for the details of API functions. `DPC++ Extension <features/DPC++_Extension.md>`_ describes how to write customized DPC++ kernels with a practical example and build it with setuptools and CMake.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/DPC++_Extension

Advanced Configuration
----------------------

The default settings for Intel® Extension for PyTorch* are sufficient for most use cases. However, if you need to customize Intel® Extension for PyTorch*, advanced configuration is available at build time and runtime.

For more detailed information, check `Advanced Configuration <features/advanced_configuration.md>`_.

A driver environment variable `ZE_FLAT_DEVICE_HIERARCHY` is currently used to select the device hierarchy model with which the underlying hardware is exposed. By default, each GPU tile is used as a device. Check the `Level Zero Specification Documentation <https://spec.oneapi.io/level-zero/latest/core/PROG.html#environment-variables>`_ for more details.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/advanced_configuration

Fully Sharded Data Parallel (FSDP)
----------------------------------

`Fully Sharded Data Parallel (FSDP)` is a PyTorch\* module that provides industry-grade solution for large model training. FSDP is a type of data parallel training, unlike DDP, where each process/worker maintains a replica of the model, FSDP shards model parameters, optimizer states and gradients across DDP ranks to reduce the GPU memory footprint used in training. This makes the training of some large-scale models feasible.

For more detailed information, check `FSDP <features/FSDP.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/FSDP

Inductor
--------
Intel® Extension for PyTorch\* now empowers users to seamlessly harness graph compilation capabilities for optimal PyTorch model performance on Intel GPU via the flagship `torch.compile <https://pytorch.org/docs/stable/generated/torch.compile.html#torch-compile>`_ API through the default "inductor" backend (`TorchInductor <https://dev-discuss.pytorch.org/t/torchinductor-a-pytorch-native-compiler-with-define-by-run-ir-and-symbolic-shapes/747/1>`_ ). 

For more detailed information, check `Inductor <features/torch_compile_gpu.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/torch_compile_gpu

Legacy Profiler Tool (Experimental)
-----------------------------------

The legacy profiler tool is an extension of PyTorch* legacy profiler for profiling operators' overhead on XPU devices. With this tool, you can get the information in many fields of the run models or code scripts. Build Intel® Extension for PyTorch* with profiler support as default and enable this tool by adding a `with` statement before the code segment.

For more detailed information, check `Legacy Profiler Tool <features/profiler_legacy.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/profiler_legacy

Simple Trace Tool (Experimental)
--------------------------------

Simple Trace is a built-in debugging tool that lets you control printing out the call stack for a piece of code. Once enabled, it can automatically print out verbose messages of called operators in a stack format with indenting to distinguish the context. 

For more detailed information, check `Simple Trace Tool <features/simple_trace.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/simple_trace

Kineto Supported Profiler Tool (Experimental)
---------------------------------------------

The Kineto supported profiler tool is an extension of PyTorch\* profiler for profiling operators' executing time cost on GPU devices. With this tool, you can get information in many fields of the run models or code scripts. Build Intel® Extension for PyTorch\* with Kineto support as default and enable this tool using the `with` statement before the code segment.

For more detailed information, check `Profiler Kineto <features/profiler_kineto.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/profiler_kineto


Compute Engine (Experimental feature for debug)
-----------------------------------------------

Compute engine is a experimental feature which provides the capacity to choose specific backend for operators with multiple implementations.

For more detailed information, check `Compute Engine <features/compute_engine.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/compute_engine

CPU-Specific
************

Operator Optimization
---------------------

Intel® Extension for PyTorch* also optimizes operators and implements several customized operators for performance boosts. A few ATen operators are replaced by their optimized counterparts in Intel® Extension for PyTorch* via the ATen registration mechanism. Some customized operators are implemented for several popular topologies. For instance, ROIAlign and NMS are defined in Mask R-CNN. To improve performance of these topologies, Intel® Extension for PyTorch* also optimized these customized operators.

.. currentmodule:: intel_extension_for_pytorch.nn
.. autoclass:: FrozenBatchNorm2d

.. currentmodule:: intel_extension_for_pytorch.nn.functional
.. autofunction:: interaction

**Auto kernel selection** is a feature that enables users to tune for better performance with GEMM operations. It is provided as parameter –auto_kernel_selection, with boolean value, of the ipex.optimize() function. By default, the GEMM kernel is computed with oneMKL primitives. However, under certain circumstances oneDNN primitives run faster. Users are able to set –auto_kernel_selection to True to run GEMM kernels with oneDNN primitives.” -> "We aim to provide good default performance by leveraging the best of math libraries and enabled weights_prepack, and it has been verified with broad set of models. If you would like to try other alternatives, you can use auto_kernel_selection toggle in ipex.optimize to switch, and you can disable weights_preack in ipex.optimize if you are concerning the memory footprint more than performance gain. However in majority cases, keeping default is what we recommend.


Runtime Extension
-----------------

Intel® Extension for PyTorch* Runtime Extension provides PyTorch frontend APIs for users to get finer-grained control of the thread runtime and provides:

- Multi-stream inference via the Python frontend module MultiStreamModule.
- Spawn asynchronous tasks from both Python and C++ frontend.
- Program core bindings for OpenMP threads from both Python and C++ frontend.

.. note:: Intel® Extension for PyTorch* Runtime extension is still in the experimental stage. The API is subject to change. More detailed descriptions are available in the `API Documentation <api_doc.html>`_.

For more detailed information, check `Runtime Extension <features/runtime_extension.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/runtime_extension


Codeless Optimization (Experimental, *NEW feature in 1.13.\**)
--------------------------------------------------------------

This feature enables users to get performance benefits from Intel® Extension for PyTorch* without changing Python scripts. It hopefully eases the usage and has been verified working well with broad scope of models, though in few cases there could be small overhead comparing to applying optimizations with Intel® Extension for PyTorch* APIs.

For more detailed information, check `Codeless Optimization <features/codeless_optimization.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/codeless_optimization.md


Graph Capture (Experimental, *NEW feature in 1.13.0\**)
-------------------------------------------------------

Since graph mode is key for deployment performance, this feature automatically captures graphs based on set of technologies that PyTorch supports, such as TorchScript and TorchDynamo. Users won't need to learn and try different PyTorch APIs to capture graphs, instead, they can turn on a new boolean flag `--graph_mode` (default off) in `ipex.optimize` to get the best of graph optimization.

For more detailed information, check `Graph Capture <features/graph_capture.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/graph_capture


HyperTune (Experimental, *NEW feature in 1.13.0\**)
---------------------------------------------------

HyperTune is an experimental feature to perform hyperparameter/execution configuration searching. The searching is used in various areas such as optimization of hyperparameters of deep learning models. The searching is extremely useful in real situations when the number of hyperparameters, including configuration of script execution, and their search spaces are huge that manually tuning these hyperparameters/configuration is impractical and time consuming. Hypertune automates this process of execution configuration searching for the `launcher <performance_tuning/launch_script.md>`_ and Intel® Extension for PyTorch*.

For more detailed information, check `HyperTune <features/hypertune.md>`_.

.. toctree::
   :hidden:
   :maxdepth: 1

   features/hypertune
