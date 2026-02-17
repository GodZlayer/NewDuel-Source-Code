#include "../Include/RMeshNode.h"
#include "../Include/RMesh.h"

namespace RealSpace3 {

RMeshNode::RMeshNode(const std::string& name) : m_name(name) {
    m_localMatrix = XMMatrixIdentity();
    m_combinedMatrix = XMMatrixIdentity();
}

void RMeshNode::UpdateMatrices(const XMMATRIX& parentMatrix) {
    if (m_isDirty) {
        XMMATRIX translation = XMMatrixTranslation(m_position.x, m_position.y, m_position.z);
        XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_rotation.x, m_rotation.y, m_rotation.z);
        XMMATRIX scale = XMMatrixScaling(m_scale.x, m_scale.y, m_scale.z);
        m_localMatrix = scale * rotation * translation;
        m_isDirty = false;
    }
    m_combinedMatrix = m_localMatrix * parentMatrix;
    for (auto& child : m_children) child->UpdateMatrices(m_combinedMatrix);
}

void RMeshNode::Draw(ID3D11DeviceContext* context, ID3D11Buffer* cb) {
    if (m_vertexBuffer && m_indexCount > 0 && cb) {
        XMMATRIX worldT = XMMatrixTranspose(m_combinedMatrix);
        context->UpdateSubresource(cb, 0, NULL, &worldT, 0, 0);

        // VINCULAR TEXTURA REAL
        if (m_pTextureSRV) {
            context->PSSetShaderResources(0, 1, &m_pTextureSRV);
        } else {
            // Se nao tiver textura, desvincula para o shader usar a cor cinza
            ID3D11ShaderResourceView* nullSRV = nullptr;
            context->PSSetShaderResources(0, 1, &nullSRV);
        }

        UINT stride = sizeof(RVertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
        context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->DrawIndexed(m_indexCount, 0, 0);
    }
    for (auto& child : m_children) child->Draw(context, cb);
}

}
