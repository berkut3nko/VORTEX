engine/
├── cmake/                   // Dependencies.cmake
├── shaders/                 // GLSL/HLSL шейдери
├── src/                     // Весь вихідний код тут
│   ├── core/            // Базовий модуль (vortex.core)
│   │   ├── Core.cppm    // export module vortex.core;
│   │   ├── Logger.cppm  // Логування
│   │   └── Profiler.cppm // Макроси профілювання (Tracy/Time)
│   │   
│   ├── math/            // Математика (vortex.math)
│   │   ├── Math.cppm    // export module vortex.math;
│   │   └── VoxelMath.cppm // Специфічна математика координат чанків
│   │   
│   ├── voxel/           // ВАШ ОСНОВНИЙ ФОКУС (vortex.voxel)
│   │   ├── Voxel.cppm   // Головний файл: export module vortex.voxel;
│   │   ├── Data.cppm    // Структури даних: Chunk, Palette, VoxelId
│   │   ├── meshing/     // Алгоритми мешингу (ізольовані!)
│   │   │   ├── MesherInterface.cppm
│   │   │   ├── Greedy.cpp    // Реалізація Greedy Meshing
│   │   │   ├── Marching.cpp  // Реалізація Marching Cubes
│   │   │   └── Naive.cpp     // Простий алгоритм для тестів
│   │   └── storage/     // Збереження/завантаження чанків
│   │   
│   ├── graphics/        // Рендер (vortex.graphics)
│   │   ├── Graphics.cppm
│   │   ├── rhi/         // Vulkan абстракції (щоб не смітити в логіці)
│   │   ├── pipelines/   // VoxelPipeline, DebugPipeline
│   │   └── internal/   // VoxelPipeline, DebugPipeline
│   │       └── VulkanContext.cpp
│   │   
│   └── physics/         // Фізика (vortex.physics)
│       ├── Physics.cppm // Інтеграція Jolt
│       └── Body.cppm    // Обгортки над Jolt BodyID
│        
└── main.cpp             // Точка входу
│
└── tests/                   // Тести (GoogleTest)