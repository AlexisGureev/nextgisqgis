/***************************************************************************
  qgschunkedentity_p.cpp
  --------------------------------------
  Date                 : July 2017
  Copyright            : (C) 2017 by Martin Dobias
  Email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgschunkedentity_p.h"

#include <QElapsedTimer>
#include <QVector4D>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <Qt3DRender/QBuffer>
typedef Qt3DRender::QBuffer Qt3DQBuffer;
#else
#include <Qt3DCore/QBuffer>
typedef Qt3DCore::QBuffer Qt3DQBuffer;
#endif

#include "qgs3dutils.h"
#include "qgschunkboundsentity_p.h"
#include "qgschunklist_p.h"
#include "qgschunkloader_p.h"
#include "qgschunknode_p.h"
#include "qgstessellatedpolygongeometry.h"

#include "qgseventtracing.h"

#include <queue>

///@cond PRIVATE


static float screenSpaceError( QgsChunkNode *node, const QgsChunkedEntity::SceneState &state )
{
  if ( node->error() <= 0 ) //it happens for meshes
    return 0;

  float dist = node->bbox().distanceFromPoint( state.cameraPos );

  // TODO: what to do when distance == 0 ?

  float sse = Qgs3DUtils::screenSpaceError( node->error(), dist, state.screenSizePx, state.cameraFov );
  return sse;
}

QgsChunkedEntity::QgsChunkedEntity( float tau, QgsChunkLoaderFactory *loaderFactory, bool ownsFactory, int primitiveBudget, Qt3DCore::QNode *parent )
  : Qgs3DMapSceneEntity( parent )
  , mTau( tau )
  , mChunkLoaderFactory( loaderFactory )
  , mOwnsFactory( ownsFactory )
  , mPrimitivesBudget( primitiveBudget )
{
  mRootNode = loaderFactory->createRootNode();
  mChunkLoaderQueue = new QgsChunkList;
  mReplacementQueue = new QgsChunkList;
}


QgsChunkedEntity::~QgsChunkedEntity()
{
  // derived classes have to make sure that any pending active job has finished / been canceled
  // before getting to this destructor - here it would be too late to cancel them
  // (e.g. objects required for loading/updating have been deleted already)
  Q_ASSERT( mActiveJobs.isEmpty() );

  // clean up any pending load requests
  while ( !mChunkLoaderQueue->isEmpty() )
  {
    QgsChunkListEntry *entry = mChunkLoaderQueue->takeFirst();
    QgsChunkNode *node = entry->chunk;

    if ( node->state() == QgsChunkNode::QueuedForLoad )
      node->cancelQueuedForLoad();
    else if ( node->state() == QgsChunkNode::QueuedForUpdate )
      node->cancelQueuedForUpdate();
    else
      Q_ASSERT( false );  // impossible!
  }

  delete mChunkLoaderQueue;

  while ( !mReplacementQueue->isEmpty() )
  {
    QgsChunkListEntry *entry = mReplacementQueue->takeFirst();

    // remove loaded data from node
    entry->chunk->unloadChunk(); // also deletes the entry
  }

  delete mReplacementQueue;
  delete mRootNode;

  if ( mOwnsFactory )
  {
    delete mChunkLoaderFactory;
  }
}

void QgsChunkedEntity::handleSceneUpdate( const SceneState &state )
{
  if ( !mIsValid )
    return;

  // Let's start the update by removing from loader queue chunks that
  // would get frustum culled if loaded (outside of the current view
  // of the camera). Removing them keeps the loading queue shorter,
  // and we avoid loading chunks that we only wanted for a short period
  // of time when camera was moving.
  pruneLoaderQueue( state );

  QElapsedTimer t;
  t.start();

  int oldJobsCount = pendingJobsCount();

  QSet<QgsChunkNode *> activeBefore = qgis::listToSet( mActiveNodes );
  mActiveNodes.clear();
  mFrustumCulled = 0;
  mCurrentTime = QTime::currentTime();

  update( mRootNode, state );

#ifdef QGISDEBUG
  int enabled = 0, disabled = 0, unloaded = 0;
#endif

  for ( QgsChunkNode *node : std::as_const( mActiveNodes ) )
  {
    if ( activeBefore.contains( node ) )
    {
      activeBefore.remove( node );
    }
    else
    {
      if ( !node->entity() )
      {
        QgsDebugError( "Active node has null entity - this should never happen!" );
        continue;
      }
      node->entity()->setEnabled( true );
#ifdef QGISDEBUG
      ++enabled;
#endif
    }
  }

  // disable those that were active but will not be anymore
  for ( QgsChunkNode *node : activeBefore )
  {
    if ( !node->entity() )
    {
      QgsDebugError( "Active node has null entity - this should never happen!" );
      continue;
    }
    node->entity()->setEnabled( false );
#ifdef QGISDEBUG
    ++disabled;
#endif
  }

  double usedGpuMemory = QgsChunkedEntity::calculateEntityGpuMemorySize( this );

  // unload those that are over the limit for replacement
  // TODO: what to do when our cache is too small and nodes are being constantly evicted + loaded again
  while ( usedGpuMemory > mGpuMemoryLimit )
  {
    QgsChunkListEntry *entry = mReplacementQueue->takeLast();
    usedGpuMemory -= QgsChunkedEntity::calculateEntityGpuMemorySize( entry->chunk->entity() );
    mActiveNodes.removeOne( entry->chunk );
    entry->chunk->unloadChunk();  // also deletes the entry
#ifdef QGISDEBUG
    ++unloaded;
#endif
  }

  if ( mBboxesEntity )
  {
    QList<QgsAABB> bboxes;
    for ( QgsChunkNode *n : std::as_const( mActiveNodes ) )
      bboxes << n->bbox();
    mBboxesEntity->setBoxes( bboxes );
  }

  // start a job from queue if there is anything waiting
  startJobs();

  mNeedsUpdate = false;  // just updated

  if ( pendingJobsCount() != oldJobsCount )
    emit pendingJobsCountChanged();

  QgsDebugMsgLevel( QStringLiteral( "update: active %1 enabled %2 disabled %3 | culled %4 | loading %5 loaded %6 | unloaded %7 elapsed %8ms" ).arg( mActiveNodes.count() )
                    .arg( enabled )
                    .arg( disabled )
                    .arg( mFrustumCulled )
                    .arg( mReplacementQueue->count() )
                    .arg( unloaded )
                    .arg( t.elapsed() ), 2 );
}

QgsRange<float> QgsChunkedEntity::getNearFarPlaneRange( const QMatrix4x4 &viewMatrix ) const
{
  QList<QgsChunkNode *> activeEntityNodes = activeNodes();

  // it could be that there are no active nodes - they could be all culled or because root node
  // is not yet loaded - we still need at least something to understand bounds of our scene
  // so lets use the root node
  if ( activeEntityNodes.empty() )
    activeEntityNodes << rootNode();

  float fnear = 1e9;
  float ffar = 0;

  for ( QgsChunkNode *node : std::as_const( activeEntityNodes ) )
  {
    // project each corner of bbox to camera coordinates
    // and determine closest and farthest point.
    QgsAABB bbox = node->bbox();
    for ( int i = 0; i < 8; ++i )
    {
      const QVector4D p( ( ( i >> 0 ) & 1 ) ? bbox.xMin : bbox.xMax,
                         ( ( i >> 1 ) & 1 ) ? bbox.yMin : bbox.yMax,
                         ( ( i >> 2 ) & 1 ) ? bbox.zMin : bbox.zMax, 1 );

      const QVector4D pc = viewMatrix * p;

      const float dst = -pc.z();  // in camera coordinates, x grows right, y grows down, z grows to the back
      fnear = std::min( fnear, dst );
      ffar = std::max( ffar, dst );
    }
  }
  return QgsRange<float>( fnear, ffar );
}

void QgsChunkedEntity::setShowBoundingBoxes( bool enabled )
{
  if ( ( enabled && mBboxesEntity ) || ( !enabled && !mBboxesEntity ) )
    return;

  if ( enabled )
  {
    mBboxesEntity = new QgsChunkBoundsEntity( this );
  }
  else
  {
    mBboxesEntity->deleteLater();
    mBboxesEntity = nullptr;
  }
}

void QgsChunkedEntity::updateNodes( const QList<QgsChunkNode *> &nodes, QgsChunkQueueJobFactory *updateJobFactory )
{
  for ( QgsChunkNode *node : nodes )
  {
    if ( node->state() == QgsChunkNode::QueuedForUpdate )
    {
      mChunkLoaderQueue->takeEntry( node->loaderQueueEntry() );
      node->cancelQueuedForUpdate();
    }
    else if ( node->state() == QgsChunkNode::Updating )
    {
      cancelActiveJob( node->updater() );
    }

    Q_ASSERT( node->state() == QgsChunkNode::Loaded );

    QgsChunkListEntry *entry = new QgsChunkListEntry( node );
    node->setQueuedForUpdate( entry, updateJobFactory );
    mChunkLoaderQueue->insertLast( entry );
  }

  // trigger update
  startJobs();
}

void QgsChunkedEntity::pruneLoaderQueue( const SceneState &state )
{
  QList<QgsChunkNode *> toRemoveFromLoaderQueue;

  // Step 1: collect all entries from chunk loader queue that would get frustum culled
  // (i.e. they are outside of the current view of the camera) and therefore loading
  // such chunks would be probably waste of time.
  QgsChunkListEntry *e = mChunkLoaderQueue->first();
  while ( e )
  {
    Q_ASSERT( e->chunk->state() == QgsChunkNode::QueuedForLoad || e->chunk->state() == QgsChunkNode::QueuedForUpdate );
    if ( Qgs3DUtils::isCullable( e->chunk->bbox(), state.viewProjectionMatrix ) )
    {
      toRemoveFromLoaderQueue.append( e->chunk );
    }
    e = e->next;
  }

  // Step 2: remove collected chunks from the loading queue
  for ( QgsChunkNode *n : toRemoveFromLoaderQueue )
  {
    mChunkLoaderQueue->takeEntry( n->loaderQueueEntry() );
    if ( n->state() == QgsChunkNode::QueuedForLoad )
    {
      n->cancelQueuedForLoad();
    }
    else  // queued for update
    {
      n->cancelQueuedForUpdate();
      mReplacementQueue->takeEntry( n->replacementQueueEntry() );
      n->unloadChunk();
    }
  }

  if ( !toRemoveFromLoaderQueue.isEmpty() )
  {
    QgsDebugMsgLevel( QStringLiteral( "Pruned %1 chunks in loading queue" ).arg( toRemoveFromLoaderQueue.count() ), 2 );
  }
}


int QgsChunkedEntity::pendingJobsCount() const
{
  return mChunkLoaderQueue->count() + mActiveJobs.count();
}

struct ResidencyRequest
{
  QgsChunkNode *node = nullptr;
  float dist = 0.0;
  int level = -1;
  ResidencyRequest() = default;
  ResidencyRequest(
    QgsChunkNode *n,
    float d,
    int l )
    : node( n )
    , dist( d )
    , level( l )
  {}
};

struct
{
  bool operator()( const ResidencyRequest &request, const ResidencyRequest &otherRequest ) const
  {
    if ( request.level == otherRequest.level )
      return request.dist > otherRequest.dist;
    return request.level > otherRequest.level;
  }
} ResidencyRequestSorter;

void QgsChunkedEntity::update( QgsChunkNode *root, const SceneState &state )
{
  QSet<QgsChunkNode *> nodes;
  QVector<ResidencyRequest> residencyRequests;

  using slotItem = std::pair<QgsChunkNode *, float>;
  auto cmp_funct = []( const slotItem & p1, const slotItem & p2 )
  {
    return p1.second <= p2.second;
  };
  int renderedCount = 0;
  std::priority_queue<slotItem, std::vector<slotItem>, decltype( cmp_funct )> pq( cmp_funct );
  pq.push( std::make_pair( root, screenSpaceError( root, state ) ) );
  while ( !pq.empty() && renderedCount <= mPrimitivesBudget )
  {
    slotItem s = pq.top();
    pq.pop();
    QgsChunkNode *node = s.first;

    if ( Qgs3DUtils::isCullable( node->bbox(), state.viewProjectionMatrix ) )
    {
      ++mFrustumCulled;
      continue;
    }

    // ensure we have child nodes (at least skeletons) available, if any
    if ( node->childCount() == -1 )
      node->populateChildren( mChunkLoaderFactory->createChildren( node ) );

    // make sure all nodes leading to children are always loaded
    // so that zooming out does not create issues
    double dist = node->bbox().center().distanceToPoint( state.cameraPos );
    residencyRequests.push_back( ResidencyRequest( node, dist, node->level() ) );

    if ( !node->entity() )
    {
      // this happens initially when root node is not ready yet
      continue;
    }
    bool becomesActive = false;

    // QgsDebugMsgLevel( QStringLiteral( "%1|%2|%3  %4  %5" ).arg( node->tileId().x ).arg( node->tileId().y ).arg( node->tileId().z ).arg( mTau ).arg( screenSpaceError( node, state ) ), 2 );
    if ( node->childCount() == 0 )
    {
      // there's no children available for this node, so regardless of whether it has an acceptable error
      // or not, it's the best we'll ever get...
      becomesActive = true;
    }
    else if ( mTau > 0 && screenSpaceError( node, state ) <= mTau )
    {
      // acceptable error for the current chunk - let's render it
      becomesActive = true;
    }
    else
    {
      // This chunk does not have acceptable error (it does not provide enough detail)
      // so we'll try to use its children. The exact logic depends on whether the entity
      // has additive strategy. With additive strategy, child nodes should be rendered
      // in addition to the parent nodes (rather than child nodes replacing parent entirely)

      if ( mAdditiveStrategy )
      {
        // Logic of the additive strategy:
        // - children that are not loaded will get requested to be loaded
        // - children that are already loaded get recursively visited
        becomesActive = true;

        QgsChunkNode *const *children = node->children();
        for ( int i = 0; i < node->childCount(); ++i )
        {
          if ( children[i]->entity() || !children[i]->hasData() )
          {
            // chunk is resident - let's visit it recursively
            pq.push( std::make_pair( children[i], screenSpaceError( children[i], state ) ) );
          }
          else
          {
            // chunk is not yet resident - let's try to load it
            if ( Qgs3DUtils::isCullable( children[i]->bbox(), state.viewProjectionMatrix ) )
              continue;

            double dist = children[i]->bbox().center().distanceToPoint( state.cameraPos );
            residencyRequests.push_back( ResidencyRequest( children[i], dist, children[i]->level() ) );
          }
        }
      }
      else
      {
        // Logic of the replace strategy:
        // - if we have all children loaded, we use them instead of the parent node
        // - if we do not have all children loaded, we request to load them and keep using the parent for the time being
        if ( node->allChildChunksResident( mCurrentTime ) )
        {
          QgsChunkNode *const *children = node->children();
          for ( int i = 0; i < node->childCount(); ++i )
            pq.push( std::make_pair( children[i], screenSpaceError( children[i], state ) ) );
        }
        else
        {
          becomesActive = true;

          QgsChunkNode *const *children = node->children();
          for ( int i = 0; i < node->childCount(); ++i )
          {
            double dist = children[i]->bbox().center().distanceToPoint( state.cameraPos );
            residencyRequests.push_back( ResidencyRequest( children[i], dist, children[i]->level() ) );
          }
        }
      }
    }

    if ( becomesActive )
    {
      mActiveNodes << node;
      // if we are not using additive strategy we need to make sure the parent primitives are not counted
      if ( !mAdditiveStrategy && node->parent() && nodes.contains( node->parent() ) )
      {
        nodes.remove( node->parent() );
        renderedCount -= mChunkLoaderFactory->primitivesCount( node->parent() );
      }
      renderedCount += mChunkLoaderFactory->primitivesCount( node );
      nodes.insert( node );
    }
  }

  // sort nodes by their level and their distance from the camera
  std::sort( residencyRequests.begin(), residencyRequests.end(), ResidencyRequestSorter );
  for ( const auto &request : residencyRequests )
    requestResidency( request.node );
}

void QgsChunkedEntity::requestResidency( QgsChunkNode *node )
{
  if ( node->state() == QgsChunkNode::Loaded || node->state() == QgsChunkNode::QueuedForUpdate || node->state() == QgsChunkNode::Updating )
  {
    Q_ASSERT( node->replacementQueueEntry() );
    Q_ASSERT( node->entity() );
    mReplacementQueue->takeEntry( node->replacementQueueEntry() );
    mReplacementQueue->insertFirst( node->replacementQueueEntry() );
  }
  else if ( node->state() == QgsChunkNode::QueuedForLoad )
  {
    // move to the front of loading queue
    Q_ASSERT( node->loaderQueueEntry() );
    Q_ASSERT( !node->loader() );
    if ( node->loaderQueueEntry()->prev || node->loaderQueueEntry()->next )
    {
      mChunkLoaderQueue->takeEntry( node->loaderQueueEntry() );
      mChunkLoaderQueue->insertFirst( node->loaderQueueEntry() );
    }
  }
  else if ( node->state() == QgsChunkNode::Loading )
  {
    // the entry is being currently processed - nothing to do really
  }
  else if ( node->state() == QgsChunkNode::Skeleton )
  {
    if ( !node->hasData() )
      return;   // no need to load (we already tried but got nothing back)

    // add to the loading queue
    QgsChunkListEntry *entry = new QgsChunkListEntry( node );
    node->setQueuedForLoad( entry );
    mChunkLoaderQueue->insertFirst( entry );
  }
  else
    Q_ASSERT( false && "impossible!" );
}


void QgsChunkedEntity::onActiveJobFinished()
{
  int oldJobsCount = pendingJobsCount();

  QgsChunkQueueJob *job = qobject_cast<QgsChunkQueueJob *>( sender() );
  Q_ASSERT( job );
  Q_ASSERT( mActiveJobs.contains( job ) );

  QgsChunkNode *node = job->chunk();

  if ( QgsChunkLoader *loader = qobject_cast<QgsChunkLoader *>( job ) )
  {
    Q_ASSERT( node->state() == QgsChunkNode::Loading );
    Q_ASSERT( node->loader() == loader );

    QgsEventTracing::addEvent( QgsEventTracing::AsyncEnd, QStringLiteral( "3D" ), QStringLiteral( "Load " ) + node->tileId().text(), node->tileId().text() );
    QgsEventTracing::addEvent( QgsEventTracing::AsyncEnd, QStringLiteral( "3D" ), QStringLiteral( "Load" ), node->tileId().text() );

    QgsEventTracing::ScopedEvent e( "3D", QString( "create" ) );
    // mark as loaded + create entity
    Qt3DCore::QEntity *entity = node->loader()->createEntity( this );

    if ( entity )
    {
      // The returned QEntity is initially enabled, so let's add it to active nodes too.
      // Soon afterwards updateScene() will be called, which would remove it from the scene
      // if the node should not be shown anymore. Ideally entities should be initially disabled,
      // but there seems to be a bug in Qt3D - if entity is disabled initially, showing it
      // by setting setEnabled(true) is not reliable (entity eventually gets shown, but only after
      // some more changes in the scene) - see https://github.com/qgis/QGIS/issues/48334
      mActiveNodes << node;

      // load into node (should be in main thread again)
      node->setLoaded( entity );

      mReplacementQueue->insertFirst( node->replacementQueueEntry() );

      emit newEntityCreated( entity );
    }
    else
    {
      node->setHasData( false );
      node->cancelLoading();
    }

    // now we need an update!
    mNeedsUpdate = true;
  }
  else
  {
    Q_ASSERT( node->state() == QgsChunkNode::Updating );
    QgsEventTracing::addEvent( QgsEventTracing::AsyncEnd, QStringLiteral( "3D" ), QStringLiteral( "Update" ), node->tileId().text() );
    node->setUpdated();
  }

  // cleanup the job that has just finished
  mActiveJobs.removeOne( job );
  job->deleteLater();

  // start another job - if any
  startJobs();

  if ( pendingJobsCount() != oldJobsCount )
    emit pendingJobsCountChanged();
}

void QgsChunkedEntity::startJobs()
{
  while ( mActiveJobs.count() < 4 )
  {
    if ( mChunkLoaderQueue->isEmpty() )
      return;

    QgsChunkListEntry *entry = mChunkLoaderQueue->takeFirst();
    Q_ASSERT( entry );
    QgsChunkNode *node = entry->chunk;
    delete entry;

    QgsChunkQueueJob *job = startJob( node );
    mActiveJobs.append( job );
  }
}

QgsChunkQueueJob *QgsChunkedEntity::startJob( QgsChunkNode *node )
{
  if ( node->state() == QgsChunkNode::QueuedForLoad )
  {
    QgsEventTracing::addEvent( QgsEventTracing::AsyncBegin, QStringLiteral( "3D" ), QStringLiteral( "Load" ), node->tileId().text() );
    QgsEventTracing::addEvent( QgsEventTracing::AsyncBegin, QStringLiteral( "3D" ), QStringLiteral( "Load " ) + node->tileId().text(), node->tileId().text() );

    QgsChunkLoader *loader = mChunkLoaderFactory->createChunkLoader( node );
    connect( loader, &QgsChunkQueueJob::finished, this, &QgsChunkedEntity::onActiveJobFinished );
    node->setLoading( loader );
    return loader;
  }
  else if ( node->state() == QgsChunkNode::QueuedForUpdate )
  {
    QgsEventTracing::addEvent( QgsEventTracing::AsyncBegin, QStringLiteral( "3D" ), QStringLiteral( "Update" ), node->tileId().text() );

    node->setUpdating();
    connect( node->updater(), &QgsChunkQueueJob::finished, this, &QgsChunkedEntity::onActiveJobFinished );
    return node->updater();
  }
  else
  {
    Q_ASSERT( false );  // not possible
    return nullptr;
  }
}

void QgsChunkedEntity::cancelActiveJob( QgsChunkQueueJob *job )
{
  Q_ASSERT( job );

  QgsChunkNode *node = job->chunk();

  if ( qobject_cast<QgsChunkLoader *>( job ) )
  {
    // return node back to skeleton
    node->cancelLoading();

    QgsEventTracing::addEvent( QgsEventTracing::AsyncEnd, QStringLiteral( "3D" ), QStringLiteral( "Load " ) + node->tileId().text(), node->tileId().text() );
    QgsEventTracing::addEvent( QgsEventTracing::AsyncEnd, QStringLiteral( "3D" ), QStringLiteral( "Load" ), node->tileId().text() );
  }
  else
  {
    // return node back to loaded state
    node->cancelUpdating();

    QgsEventTracing::addEvent( QgsEventTracing::AsyncEnd, QStringLiteral( "3D" ), QStringLiteral( "Update" ), node->tileId().text() );
  }

  job->cancel();
  mActiveJobs.removeOne( job );
  job->deleteLater();
}

void QgsChunkedEntity::cancelActiveJobs()
{
  while ( !mActiveJobs.isEmpty() )
  {
    cancelActiveJob( mActiveJobs.takeFirst() );
  }
}

double QgsChunkedEntity::calculateEntityGpuMemorySize( Qt3DCore::QEntity *entity )
{
  long long usedGpuMemory = 0;
  for ( Qt3DQBuffer *buffer : entity->findChildren<Qt3DQBuffer *>() )
  {
    usedGpuMemory += buffer->data().size();
  }
  for ( Qt3DRender::QTexture2D *tex : entity->findChildren<Qt3DRender::QTexture2D *>() )
  {
    // TODO : lift the assumption that the texture is RGBA
    usedGpuMemory += tex->width() * tex->height() * 4;
  }
  return usedGpuMemory / 1024.0 / 1024.0;
}

QVector<QgsRayCastingUtils::RayHit> QgsChunkedEntity::rayIntersection( const QgsRayCastingUtils::Ray3D &ray, const QgsRayCastingUtils::RayCastContext &context ) const
{
  Q_UNUSED( ray )
  Q_UNUSED( context )
  return QVector<QgsRayCastingUtils::RayHit>();
}

/// @endcond
