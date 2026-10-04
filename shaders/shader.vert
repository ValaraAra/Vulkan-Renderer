#version 460

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(push_constant, scalar) uniform FrameConstants
{
	uint64_t vertexBufferAddress;
	uint64_t materialBufferAddress;
	uint64_t renderItemsBufferAddress;
	mat4 viewProjection;
} frameConstants;

struct Vertex
{
	vec3 position;
	vec3 color;
	vec3 normal;
	vec2 uv;
};

layout(buffer_reference, scalar) readonly buffer VertexPtr
{
	Vertex vertices[];
};

struct Material
{
	vec4 baseColor;
	uint textureID;
};

layout(buffer_reference, scalar) readonly buffer MaterialPtr
{
	Material materials[];
};

struct RenderItem
{
	mat4 worldMatrix;
	mat3 normalMatrix;
	uint materialIndex;
};

layout(buffer_reference, scalar) readonly buffer RenderItemPtr
{
	RenderItem renderItems[];
};

layout (location = 0) out vec3 outColor;
layout (location = 1) out vec3 outNormal;
layout (location = 2) out vec2 outUV;
layout (location = 3) out flat uint outTextureIndex;
layout (location = 4) out flat vec4 outMaterialBaseColor;

void main()
{
	VertexPtr vBuffer = VertexPtr(frameConstants.vertexBufferAddress);
	Vertex vert = vBuffer.vertices[gl_VertexIndex];

	RenderItemPtr riBuffer = RenderItemPtr(frameConstants.renderItemsBufferAddress);
	RenderItem ri = riBuffer.renderItems[gl_InstanceIndex];

	MaterialPtr mBuffer = MaterialPtr(frameConstants.materialBufferAddress);
	Material mat = mBuffer.materials[ri.materialIndex];

	vec4 worldPos = ri.worldMatrix * vec4(vert.position, 1.0);
	gl_Position = frameConstants.viewProjection * worldPos;

	outColor = vert.color;
	outNormal = ri.normalMatrix * vert.normal;
	outUV = vert.uv;
	outTextureIndex = mat.textureID;
	outMaterialBaseColor = mat.baseColor;
}