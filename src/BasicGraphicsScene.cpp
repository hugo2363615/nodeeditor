#include "BasicGraphicsScene.hpp"

#include "AbstractNodeGeometry.hpp"
#include "ConnectionGraphicsObject.hpp"
#include "ConnectionIdUtils.hpp"
#include "DefaultHorizontalNodeGeometry.hpp"
#include "DefaultNodePainter.hpp"
#include "DefaultVerticalNodeGeometry.hpp"
#include "GraphicsView.hpp"
#include "NodeGraphicsObject.hpp"

#include <QUndoStack>

#include <QtWidgets/QGraphicsSceneMoveEvent>
#include <QtWidgets/QFileDialog>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QtGlobal>

#include <queue>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <utility>


namespace QtNodes
{

BasicGraphicsScene::
BasicGraphicsScene(AbstractGraphModel &graphModel,
                   QObject *   parent)
  : QGraphicsScene(parent)
  , _graphModel(graphModel)
  , _nodeGeometry(std::make_unique<DefaultHorizontalNodeGeometry>(_graphModel))
  , _nodePainter(std::make_unique<DefaultNodePainter>())
  , _undoStack(new QUndoStack(this))
  , _orientation(Qt::Horizontal)
{
  setItemIndexMethod(QGraphicsScene::NoIndex);


  connect(&_graphModel, &AbstractGraphModel::connectionCreated,
          this, &BasicGraphicsScene::onConnectionCreated);

  connect(&_graphModel, &AbstractGraphModel::connectionDeleted,
          this, &BasicGraphicsScene::onConnectionDeleted);

  connect(&_graphModel, &AbstractGraphModel::nodeCreated,
          this, &BasicGraphicsScene::onNodeCreated);

  connect(&_graphModel, &AbstractGraphModel::nodeDeleted,
          this, &BasicGraphicsScene::onNodeDeleted);

  connect(&_graphModel, &AbstractGraphModel::nodePositionUpdated,
          this, &BasicGraphicsScene::onNodePositionUpdated);

  connect(&_graphModel, &AbstractGraphModel::nodeUpdated,
          this, &BasicGraphicsScene::onNodeUpdated);

  connect(&_graphModel, &AbstractGraphModel::modelReset,
          this, &BasicGraphicsScene::onModelReset);

  traverseGraphAndPopulateGraphicsObjects();
}


BasicGraphicsScene::
~BasicGraphicsScene() = default;


AbstractGraphModel const &
BasicGraphicsScene::
graphModel() const
{
  return _graphModel;
}


AbstractGraphModel &
BasicGraphicsScene::
graphModel()
{
  return _graphModel;
}


AbstractNodeGeometry &
BasicGraphicsScene::
nodeGeometry()
{
  return *_nodeGeometry;
}


AbstractNodePainter &
BasicGraphicsScene::
nodePainter()
{
  return *_nodePainter;
}


void
BasicGraphicsScene::
setNodePainter(std::unique_ptr<AbstractNodePainter> newPainter)
{
  _nodePainter = std::move(newPainter);
}


QUndoStack &
BasicGraphicsScene::
undoStack()
{
  return *_undoStack;
}


std::unique_ptr<ConnectionGraphicsObject> const &
BasicGraphicsScene::
makeDraftConnection(ConnectionId const incompleteConnectionId)
{
  _draftConnection =
    std::make_unique<ConnectionGraphicsObject>(*this,
                                               incompleteConnectionId);

  _draftConnection->grabMouse();

  return _draftConnection;
}


void
BasicGraphicsScene::
resetDraftConnection()
{
  _draftConnection.reset();
}


void
BasicGraphicsScene::
clearScene()
{
  auto const &allNodeIds =
    graphModel().allNodeIds();

  for ( auto nodeId : allNodeIds)
  {
    graphModel().deleteNode(nodeId);
  }
}


NodeGraphicsObject*
BasicGraphicsScene::
nodeGraphicsObject(NodeId nodeId)
{
  NodeGraphicsObject * ngo = nullptr;
  auto it = _nodeGraphicsObjects.find(nodeId);
  if (it != _nodeGraphicsObjects.end())
  {
    ngo = it->second.get();
  }

  return ngo;
}


ConnectionGraphicsObject*
BasicGraphicsScene::
connectionGraphicsObject(ConnectionId connectionId)
{
  ConnectionGraphicsObject * cgo = nullptr;
  auto it = _connectionGraphicsObjects.find(connectionId);
  if (it != _connectionGraphicsObjects.end())
  {
    cgo = it->second.get();
  }

  return cgo;
}


void
BasicGraphicsScene::
setOrientation(Qt::Orientation const orientation)
{
  if (_orientation != orientation)
  {
    _orientation = orientation;

    switch(_orientation)
    {
      case Qt::Horizontal:
        _nodeGeometry = std::make_unique<DefaultHorizontalNodeGeometry>(_graphModel);
        break;

      case Qt::Vertical:
        _nodeGeometry = std::make_unique<DefaultVerticalNodeGeometry>(_graphModel);
        break;
    }

    onModelReset();
  }
}


QMenu *
BasicGraphicsScene::
createSceneMenu(QPointF const scenePos)
{
  Q_UNUSED(scenePos);
  return nullptr;
}


void
BasicGraphicsScene::
traverseGraphAndPopulateGraphicsObjects()
{
  auto allNodeIds = _graphModel.allNodeIds();

  std::vector<ConnectionId> connectionsToCreate;

  while (!allNodeIds.empty())
  {
    std::queue<NodeId> fifo;

    auto firstId = *allNodeIds.begin();
    allNodeIds.erase(firstId);

    fifo.push(firstId);

    while (!fifo.empty())
    {
      auto nodeId = fifo.front();
      fifo.pop();

      _nodeGraphicsObjects[nodeId] =
        std::make_unique<NodeGraphicsObject>(*this, nodeId);

      unsigned int nOutPorts =
        _graphModel.nodeData(nodeId, NodeRole::OutPortCount).toUInt();

      for (PortIndex index = 0; index < nOutPorts; ++index)
      {
        auto const& conns =
          _graphModel.connections(nodeId,
                                  PortType::Out,
                                  index);

        for (auto cn : conns)
        {
          fifo.push(cn.inNodeId);

          allNodeIds.erase(cn.inNodeId);

          connectionsToCreate.push_back(cn);
        }
      }
    } // while
  }

  for (auto const & connectionId : connectionsToCreate)
  {
    _connectionGraphicsObjects[connectionId] =
      std::make_unique<ConnectionGraphicsObject>(*this,
                                                 connectionId);
  }
}


void
BasicGraphicsScene::
updateAttachedNodes(ConnectionId const connectionId,
                    PortType const portType)
{
  auto node = nodeGraphicsObject(getNodeId(portType, connectionId));

  if (node)
  {
    node->update();
  }
}


void
BasicGraphicsScene::
onConnectionDeleted(ConnectionId const connectionId)
{
  auto it = _connectionGraphicsObjects.find(connectionId);
  if (it != _connectionGraphicsObjects.end())
  {
    _connectionGraphicsObjects.erase(it);
  }

  // TODO: do we need it?
  if (_draftConnection &&
      _draftConnection->connectionId() == connectionId)
  {
    _draftConnection.reset();
  }

  updateAttachedNodes(connectionId, PortType::Out);
  updateAttachedNodes(connectionId, PortType::In);
}


void
BasicGraphicsScene::
onConnectionCreated(ConnectionId const connectionId)
{
  _connectionGraphicsObjects[connectionId] =
    std::make_unique<ConnectionGraphicsObject>(*this,
                                               connectionId);

  updateAttachedNodes(connectionId, PortType::Out);
  updateAttachedNodes(connectionId, PortType::In);
}


void
BasicGraphicsScene::
onNodeDeleted(NodeId const nodeId)
{
  auto it = _nodeGraphicsObjects.find(nodeId);
  if (it != _nodeGraphicsObjects.end())
  {
    _nodeGraphicsObjects.erase(it);
  }
}


void
BasicGraphicsScene::
onNodeCreated(NodeId const nodeId)
{
  _nodeGraphicsObjects[nodeId] =
    std::make_unique<NodeGraphicsObject>(*this, nodeId);
}


void
BasicGraphicsScene::
onNodePositionUpdated(NodeId const nodeId)
{
  auto node = nodeGraphicsObject(nodeId);
  if (node)
  {
    node->setPos(_graphModel.nodeData(nodeId,
                                      NodeRole::Position).value<QPointF>());
    node->update();
  }
}


void
BasicGraphicsScene::
onNodeUpdated(NodeId const nodeId)
{
  auto node = nodeGraphicsObject(nodeId);

  if (node)
  {
    node->setGeometryChanged();

    _nodeGeometry->recomputeSize(nodeId);

    node->update();
    node->moveConnections();
  }
}


void
BasicGraphicsScene::
onModelReset()
{
  _connectionGraphicsObjects.clear();
  _nodeGraphicsObjects.clear();

  clear();

  traverseGraphAndPopulateGraphicsObjects();
}

}
