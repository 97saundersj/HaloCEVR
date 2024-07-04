#pragma once
#include "Maths/Vectors.h"
#include "Maths/Matrices.h"
#include <vector>
#include <d3d9.h>


class DebugRenderer
{
public:
	// Debug API functions
	void DrawLine2D(struct Vector2& start, struct Vector2& end, D3DCOLOR color);
	void DrawLine3D(struct Vector3& start, struct Vector3& end, D3DCOLOR color, bool bRespectDepth = true, float thickness = 0.05f);
	void DrawCoordinate(struct Vector3& pos, class Matrix3& rot, float size = 0.05f, bool bRespectDepth = true);
	void DrawRenderTarget(struct IDirect3DTexture9* renderTarget, struct Vector3& pos, class Matrix3& rot, struct Vector2& size, bool bRespectDepth = true);

	// Core functions
	void ExtractMatrices(struct Renderer* playerRenderer);
	void Render(struct IDirect3DDevice9* pDevice);
	void PostRender();
protected:

	void Draw2DLines(struct IDirect3DDevice9* pDevice);
	void Draw3DLines(struct IDirect3DDevice9* pDevice);
	void DrawRenderTargets(struct IDirect3DDevice9* pDevice);

	static constexpr int MAX_LINES = 32;

	struct VertexData2D
	{
		float x, y, z, rhw;
		DWORD color;
	};

	struct VertexData3D
	{
		float x, y, z;
		DWORD color;
	};

	struct VertexDataTex
	{
		float x, y, z;
		float u, v;
	};

	struct RenderTarget
	{
		Vector3 pos;
		Matrix3 rot;
		Vector2 size;
		struct IDirect3DTexture9* texture;
		bool bRespectDepth;
	};

	VertexData2D vertices2D[MAX_LINES];
	VertexData3D vertices3D[MAX_LINES];
	int vertex2DCount = 0;
	int vertex3DCount = 0;

	std::vector<VertexData2D> lines2D;
	std::vector<VertexData3D> lines3D;
	std::vector<VertexData3D> depthLines3D;
	std::vector<RenderTarget> renderTargets;

	// MVP matrices the current camera is using, must match exactly or 3D elements will be misaligned
	D3DMATRIX world;
	D3DMATRIX view;
	D3DMATRIX projection;
};
