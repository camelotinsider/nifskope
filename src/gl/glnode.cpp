/***** BEGIN LICENSE BLOCK *****

BSD License

Copyright (c) 2005-2015, NIF File Format Library and Tools
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the NIF File Format Library and Tools project may not be
   used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***** END LICENCE BLOCK *****/

#include "glnode.h"

#include "nifskope.h"
#include "gl/controllers.h"
#include "gl/glmarker.h"
#include "gl/glscene.h"
#include "gl/marker/furniture.h"
#include "gl/marker/constraints.h"
#include "model/nifmodel.h"
#include "ui/settingsdialog.h"
#include "qtcompat.h"
#include "glview.h"

#include "lib/nvtristripwrapper.h"

#include <QRegularExpression>
#include <QSettings>

#include <algorithm> // std::stable_sort

//! @file glnode.cpp Scene management for visible NiNodes and their children.

int Node::SELECTING = 0;

static QColor highlightColor;
static QColor wireframeColor;

/*
 *  Node list
 */

NodeList::NodeList()
{
}

NodeList::NodeList( const NodeList & other )
{
	operator=( other );
}

NodeList::~NodeList()
{
	clear();
}

void NodeList::clear()
{
	foreach ( Node * n, nodes ) {
		del( n );
	}
}

NodeList & NodeList::operator=( const NodeList & other )
{
	clear();
	for ( Node * n : other.list() ) {
		add( n );
	}
	return *this;
}

void NodeList::add( Node * n )
{
	if ( n && !nodes.contains( n ) ) {
		++n->ref;
		nodes.append( n );
	}
}

void NodeList::del( Node * n )
{
	if ( nodes.contains( n ) ) {
		int cnt = nodes.removeAll( n );

		if ( n->ref <= cnt ) {
			delete n;
		} else {
			n->ref -= cnt;
		}
	}
}

Node * NodeList::get( const QModelIndex & index ) const
{
	for ( Node * n : nodes ) {
		if ( n->index().isValid() && n->index() == index )
			return n;
	}
	return nullptr;
}

void NodeList::validate()
{
	QList<Node *> rem;
	for ( Node * n : nodes ) {
		if ( !n->isValid() )
			rem.append( n );
	}
	foreach ( Node * n, rem ) {
		del( n );
	}
}

static bool compareNodes( const Node * node1, const Node * node2 )
{
	return node1->id() < node2->id();
}

static bool compareNodesAlpha( const Node * node1, const Node * node2 )
{
	// Presorted meshes override other sorting
	// Alpha enabled meshes on top (sorted from rear to front)

	bool p1 = node1->isPresorted();
	bool p2 = node2->isPresorted();

	// Presort meshes
	if ( p1 && p2 ) {
		return node1->id() < node2->id();
	}

	bool a1 = node1->findProperty<AlphaProperty>();
	bool a2 = node2->findProperty<AlphaProperty>();

	float d1 = node1->viewDepth();
	float d2 = node2->viewDepth();

	// Alpha sort meshes
	if ( a1 == a2 ) {
		return (d1 < d2);
	}

	return a2;
}

void NodeList::orderedNodeSort()
{
	for ( Node * node : nodes )
		node->presorted = true;
	std::stable_sort( nodes.begin(), nodes.end(), compareNodes );
}

void NodeList::alphaSort()
{
	std::stable_sort( nodes.begin(), nodes.end(), compareNodesAlpha );
}

/*
 *	Node
 */


Node::Node( Scene * s, const QModelIndex & iBlock) : IControllable( s, iBlock ), parent( 0 ), ref( 0 )
{
	nodeId = 0;
	flags.bits = 0;

	updateSettings();

	connect( NifSkope::getOptions(), &SettingsDialog::saveSettings, this, &Node::updateSettings );
}

void Node::updateSettings()
{
	QSettings settings;
	settings.beginGroup( "Settings/Render/Colors/" );
	// TODO: Remove the registry read for every new Node
	cfg.highlight = settings.value( "Highlight", QColor( 255, 255, 0 ) ).value<QColor>();
	cfg.wireframe = settings.value( "Wireframe", QColor( 0, 255, 0 ) ).value<QColor>();

	highlightColor = cfg.highlight;
	wireframeColor = cfg.wireframe;

	settings.endGroup();
}

// Old Options API
//	TODO: Move away from the GL-like naming
void glHighlightColor()
{
	glColor( Color4( highlightColor ) );
}

void glNormalColor()
{
	glColor( Color4( wireframeColor ) );
}

void Node::glHighlightColor() const
{
	glColor( Color4( cfg.highlight ) );
}

void Node::glNormalColor() const
{
	glColor( Color4( cfg.wireframe ) );
}


void Node::clear()
{
	IControllable::clear();

	nodeId = 0;
	flags.bits = 0;
	local = Transform();

	children.clear();
	properties.clear();
}

Controller * Node::findController( const QString & proptype, const QString & ctrltype, const QString & var1, const QString & var2 )
{
	if ( proptype != "<empty>" && !proptype.isEmpty() ) {
		for ( Property * prp : properties ) {
			if ( prp->typeId() == proptype ) {
				return prp->findController( ctrltype, var1, var2 );
			}
		}
		return nullptr;
	}

	return IControllable::findController( ctrltype, var1, var2 );
}

Controller * Node::findController( const QString & proptype, const QModelIndex & index )
{
	Controller * c = nullptr;

	for ( Property * prp : properties ) {
		if ( prp->typeId() == proptype ) {
			if ( c )
				break;

			c = prp->findController( index );
		}
	}

	return c;
}

void Node::updateImpl( const NifModel * nif, const QModelIndex & index )
{
	IControllable::updateImpl( nif, index );

	nodeId = nif->getBlockNumber( iBlock );

	if ( iBlock == index ) {
		flags.bits = nif->get<int>( iBlock, "Flags" );
		local = Transform( nif, iBlock );
		// BSOrderedNode support
		//	Only set if true (|=) so that it propagates to all children
		if ( nif->getBlockIndex( iBlock, "BSOrderedNode" ).isValid() )
			presorted = true;

		// Properties
		properties.clear();
		for ( auto l : nif->getLinkArray(iBlock, "Properties") )
			properties.add( scene->getProperty(nif, nif->getBlockIndex(l)) );

		properties.add( scene->getProperty(nif, iBlock, "Shader Property", "BSShaderProperty") );
		properties.add( scene->getProperty(nif, iBlock, "Alpha Property", "NiAlphaProperty") );

		// Children
		children.clear();
		QModelIndex iChildren = nif->getIndex( iBlock, "Children" );
		if ( iChildren.isValid() ) {
			int nChildren = nif->rowCount(iChildren);
			if ( nChildren > 0 ) {
				QList<qint32> lChildren = nif->getChildLinks( nodeId );
				for ( int c = 0; c < nChildren; c++ ) {
					qint32 link = nif->getLink( QModelIndex_child( iChildren, c ) );

					if ( lChildren.contains( link ) ) {
						QModelIndex iChild = nif->getBlockIndex( link );
						Node * node = scene->getNode( nif, iChild );
						if ( node )
							node->makeParent( this );
					}
				}
			}
		}
	}
}

void Node::makeParent( Node * newParent )
{
	if ( parent )
		parent->children.del( this );

	parent = newParent;

	if ( parent )
		parent->children.add( this );
}

void Node::setController( const NifModel * nif, const QModelIndex & iController )
{
	QString cname = nif->itemName( iController );

	if ( cname == "NiTransformController" ) {
		Controller * ctrl = new TransformController( this, iController );
		registerController(nif, ctrl);
	} else if ( cname == "NiMultiTargetTransformController" ) {
		Controller * ctrl = new MultiTargetTransformController( this, iController );
		registerController(nif, ctrl);
	} else if ( cname == "NiControllerManager" ) {
		Controller * ctrl = new ControllerManager( this, iController );
		registerController(nif, ctrl);
	} else if ( cname == "NiKeyframeController" ) {
		Controller * ctrl = new KeyframeController( this, iController );
		registerController(nif, ctrl);
	} else if ( cname == "NiVisController" ) {
		Controller * ctrl = new VisibilityController( this, iController );
		registerController(nif, ctrl);
	}
}

void Node::activeProperties( PropertyList & list ) const
{
	list.merge( properties );

	if ( parent )
		parent->activeProperties( list );
}

const Transform & Node::viewTrans() const
{
	if ( scene->viewTrans.contains( nodeId ) )
		return scene->viewTrans[ nodeId ];

	Transform t;

	if ( parent )
		t = parent->viewTrans() * local;
	else
		t = scene->view * worldTrans();

	scene->viewTrans.insert( nodeId, t );
	return scene->viewTrans[ nodeId ];
}

const Transform & Node::worldTrans() const
{
	if ( scene->worldTrans.contains( nodeId ) )
		return scene->worldTrans[ nodeId ];

	Transform t = local;

	if ( parent )
		t = parent->worldTrans() * t;

	scene->worldTrans.insert( nodeId, t );
	return scene->worldTrans[ nodeId ];
}

Transform Node::localTrans( int root ) const
{
	Transform trans;
	const Node * node = this;

	while ( node && node->nodeId != root ) {
		trans = node->local * trans;
		node  = node->parent;
	}

	return trans;
}

const Vector3 Node::center() const
{
	return worldTrans().translation;
}

float Node::viewDepth() const
{
	return viewTrans().translation[2];
}

Node * Node::findParent( int id ) const
{
	Node * node = parent;

	while ( node && node->nodeId != id )
		node = node->parent;

	return node;
}

Node * Node::findChild( int id ) const
{
	for ( Node * child : children.list() ) {
		if ( child ) {
			if ( child->nodeId == id )
				return child;

			child = child->findChild( id );
			if ( child )
				return child;
		}
	}
	return nullptr;
}

Node * Node::findChild( const QString & str ) const
{
	if ( this->name == str )
		return const_cast<Node *>( this );

	for ( Node * child : children.list() ) {
		Node * n = child->findChild( str );

		if ( n )
			return n;
	}
	return nullptr;
}

bool Node::isHidden() const
{
	if ( scene->hasOption(Scene::ShowHidden) )
		return false;
	if ( flags.node.hidden )
		return true;
	if ( parent && parent->isHidden() )
		return true;
	return false; /*!Options::cullExpression().pattern().isEmpty() && name.contains( Options::cullExpression() );*/
}

void Node::transform()
{
	IControllable::transform();

	// if there's a rigid body attached, then calculate and cache the body's transform
	// (need this later in the drawing stage for the constraints)
	auto nif = NifModel::fromValidIndex( iBlock );
	if ( nif && nif->getBSVersion() > 0 ) {
		QModelIndex iObject = nif->getBlockIndex( nif->getLink( iBlock, "Collision Object" ) );
		if ( iObject.isValid() ) {
			QModelIndex iBody = nif->getBlockIndex( nif->getLink( iObject, "Body" ) );

			if ( iBody.isValid() ) {
				Transform t;
				t.scale = bhkScale( nif );

				if ( nif->isNiBlock( iBody, "bhkRigidBodyT" ) ) {
					auto cinfo = nif->getIndex( iBody, "Rigid Body Info" );
					t.rotation.fromQuat( nif->get<Quat>( cinfo, "Rotation" ) );
					t.translation = Vector3( nif->get<Vector4>( cinfo, "Translation" ) * bhkScale( nif ) );
				}

				scene->bhkBodyTrans.insert( nif->getBlockNumber( iBody ), worldTrans() * t );
			}
		}
	}

	for ( Node * node : children.list() ) {
		node->transform();
	}
}

void Node::transformShapes()
{
	for ( Node * node : children.list() ) {
		node->transformShapes();
	}
}

void Node::draw()
{
	if ( isHidden() || iBlock == scene->currentBlock )
		return;

	if ( !scene->isSelModeObject() )
		return;

	if ( Node::SELECTING ) {
		int s_nodeId = ID2COLORKEY( nodeId );
		glColor4ubv( (GLubyte *)&s_nodeId );
		glLineWidth( GLView::Settings::lineWidthSelect );	// make hitting a line a litlle bit more easy
	} else {
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LEQUAL );
		glDepthMask( GL_TRUE );
		glDisable( GL_TEXTURE_2D );
		glDisable( GL_NORMALIZE );
		glDisable( GL_LIGHTING );
		glDisable( GL_COLOR_MATERIAL );
		glEnable( GL_BLEND );
		glDisable( GL_ALPHA_TEST );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

		glNormalColor();
		glLineWidth( GLView::Settings::lineWidthHighlight );
	}

	glPointSize( GLView::Settings::vertexSelectPointSize );

	Vector3 a = viewTrans().translation;
	Vector3 b = a;

	if ( parent )
		b = parent->viewTrans().translation;

	glBegin( GL_POINTS );
	glVertex( a );
	glEnd();

	if ( Node::SELECTING ) {
		glBegin( GL_LINES );
		glVertex( a );
		glVertex( b );
		glEnd();
	} else {
		auto c = cfg.wireframe;
		glColor4f( c.redF(), c.greenF(), c.blueF(), c.alphaF() / 3.0 );
		drawDashLine( a, b, 144 );
	}

	for ( Node * node : children.list() ) {
		node->draw();
	}
}

void Node::drawSelection() const
{
	auto nif = NifModel::fromIndex( scene->currentIndex );
	if ( !nif )
		return;

	if ( !scene->isSelModeObject() )
		return;

	bool extraData = false;
	auto currentBlock = nif->itemName( scene->currentBlock );
	if ( currentBlock == "BSConnectPoint::Parents" )
		extraData = nif->getBlockNumber( iBlock ) == 0; // Root Node only

	if ( scene->currentBlock != iBlock && !extraData )
		return;

	auto n = scene->currentIndex.data( NifSkopeDisplayRole ).toString();

	if ( Node::SELECTING ) {
		int s_nodeId = ID2COLORKEY( nodeId );
		glColor4ubv( (GLubyte *)&s_nodeId );
		glLineWidth( GLView::Settings::lineWidthSelect );
	} else {
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_ALWAYS );
		glDepthMask( GL_TRUE );
		glDisable( GL_TEXTURE_2D );
		glDisable( GL_NORMALIZE );
		glDisable( GL_LIGHTING );
		glDisable( GL_COLOR_MATERIAL );
		glEnable( GL_BLEND );
		glDisable( GL_ALPHA_TEST );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

		glHighlightColor();
		glLineWidth( GLView::Settings::lineWidthHighlight );
	}

	glPointSize( GLView::Settings::vertexSelectPointSize );

	glPushMatrix();
	glMultMatrix( viewTrans() );

	float sceneRadius = scene->bounds().radius;
	float normalScale = (sceneRadius > 150.0) ? 1.0 : sceneRadius / 150.0;

	if ( currentBlock == "BSConnectPoint::Parents" ) {
		glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

		auto cp = nif->getIndex( scene->currentBlock, "Connect Points" );
		bool isChild = scene->currentIndex.parent().data( NifSkopeDisplayRole ).toString() == "Connect Points";

		int sel = -1;
		if ( n == "Connect Points" && !nif->isArray( scene->currentIndex ) ) {
			sel = scene->currentIndex.row();
		} else if ( isChild ) {
			sel = scene->currentIndex.parent().row();
		}

		int ct = nif->rowCount( cp );
		for ( int i = 0; i < ct; i++ ) {
			auto p = QModelIndex_child( cp, i );

			auto trans = nif->get<Vector3>( p, "Translation" );
			auto rot = nif->get<Quat>( p, "Rotation" );
			//auto scale = nif->get<float>( p, "Scale" );

			Transform t;
			Matrix m;
			m.fromQuat( rot );
			t.rotation = m;
			t.translation = trans;
			t.scale = normalScale * 16;

			if ( i == sel ) {
				glHighlightColor();
			} else {
				glNormalColor();
			}

			glPushMatrix();
			glMultMatrix( t );

			auto pos = Vector3( 0, 0, 0 );

			drawDashLine( pos, Vector3( 0, 1, 0 ), 15 );
			drawDashLine( pos, Vector3( 1, 0, 0 ), 15 );
			drawDashLine( pos, Vector3( 0, 0, 1 ), 15 );
			drawCircle( pos, Vector3( 0, 1, 0 ), 1, 64 );

			glPopMatrix();
		}

	}

	if ( currentBlock.endsWith( "Node" ) && scene->hasOption(Scene::ShowNodes) && scene->hasOption(Scene::ShowAxes) ) {
		glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

		Transform t;
		t.rotation = nif->get<Matrix>( scene->currentIndex, "Rotation" );

		glPushMatrix();
		glMultMatrix( t );

		auto pos = Vector3( 0, 0, 0 );

		glColor( { 0, 1, 0 } );
		drawDashLine( pos, Vector3( 0, 1, 0 ), 15 );
		glColor( { 1, 0, 0 } );
		drawDashLine( pos, Vector3( 1, 0, 0 ), 15 );
		glColor( { 0, 0, 1 } );
		drawDashLine( pos, Vector3( 0, 0, 1 ), 15 );

		glPopMatrix();
	}

	glPopMatrix();

	if ( extraData )
		return;

	Vector3 a = viewTrans().translation;
	Vector3 b = a;

	if ( parent )
		b = parent->viewTrans().translation;

	glBegin( GL_POINTS );
	glVertex( a );
	glEnd();

	auto c = cfg.highlight;
	glColor4f( c.redF(), c.greenF(), c.blueF(), c.alphaF() * 0.8 );
	glBegin( GL_LINES );
	glVertex( a );
	glVertex( b );
	glEnd();

	for ( Node * node : children.list() ) {
		node->draw();
	}
}

void DrawVertexSelection( QVector<Vector3> & verts, int i )
{
	glPointSize( GLView::Settings::vertexPointSize );
	glDepthFunc( GL_LEQUAL );
	glNormalColor();
	glBegin( GL_POINTS );

	for ( int j = 0; j < verts.count(); j++ )
		glVertex( verts.value( j ) );

	glEnd();

	if ( i >= 0 ) {
		glDepthFunc( GL_ALWAYS );
		glHighlightColor();
		glBegin( GL_POINTS );
		glVertex( verts.value( i ) );
		glEnd();
	}
}

void DrawTriangleSelection( QVector<Vector3> const & verts, Triangle const & tri )
{
	glLineWidth( GLView::Settings::lineWidthWireframe );
	glDepthFunc( GL_ALWAYS );
	glHighlightColor();
	glBegin( GL_LINE_STRIP );
	glVertex( verts.value( tri.v1() ) );
	glVertex( verts.value( tri.v2() ) );
	glVertex( verts.value( tri.v3() ) );
	glVertex( verts.value( tri.v1() ) );
	glEnd();
}

void DrawTriangleIndex( QVector<Vector3> const & verts, Triangle const & tri, int index )
{
	Vector3 c = ( verts.value( tri.v1() ) + verts.value( tri.v2() ) + verts.value( tri.v3() ) ) /  3.0;
	renderText( c, QString( "%1" ).arg( index ) );
}

void drawHvkShape( const NifModel * nif, const QModelIndex & iShape, QStack<QModelIndex> & stack, const Scene * scene, const float origin_color3fv[3] )
{
	QString name = (nif) ? nif->itemName( iShape ) : "";

	bool extraData = (name == "hkPackedNiTriStripsData");

	if ( (!nif || !iShape.isValid() || stack.contains( iShape )) && !extraData )
		return;

	if ( !scene->isSelModeObject() )
		return;

	stack.push( iShape );

	//qDebug() << "draw shape" << nif->getBlockNumber( iShape ) << nif->itemName( iShape );

	if ( name.endsWith( "ListShape" ) ) {
		QModelIndex iShapes = nif->getIndex( iShape, "Sub Shapes" );

		if ( iShapes.isValid() ) {
			for ( int r = 0; r < nif->rowCount( iShapes ); r++ ) {
				if ( !Node::SELECTING ) {
					if ( scene->currentBlock == nif->getBlockIndex( nif->getLink( QModelIndex_child( iShapes, r ) ) ) ) {
						// fix: add selected visual to havok meshes
						glHighlightColor();
						glLineWidth( GLView::Settings::lineWidthHighlight );
					} else {
						if ( scene->currentBlock != iShape ) {
							// allow group highlighting
							glLineWidth( GLView::Settings::lineWidthWireframe * 0.625f );
							glColor3fv( origin_color3fv );
						}
					}
				}

				drawHvkShape( nif, nif->getBlockIndex( nif->getLink( QModelIndex_child( iShapes, r ) ) ), stack, scene, origin_color3fv );
			}
		}
	} else if ( name == "bhkTransformShape" || name == "bhkConvexTransformShape" ) {
		glPushMatrix();
		Matrix4 tm = nif->get<Matrix4>( iShape, "Transform" );
		// TODO find a better way to apply tm
		Transform t;
		Vector3 s;
		tm.decompose( t.translation, t.rotation, s );
		t.scale = (s[0] + s[1] + s[2]) / 3.0; // assume uniform
		glMultMatrix( t );
		drawHvkShape( nif, nif->getBlockIndex( nif->getLink( iShape, "Shape" ) ), stack, scene, origin_color3fv );
		glPopMatrix();
	} else if ( name == "bhkSphereShape" ) {
		if ( Node::SELECTING ) {
			int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iShape ) );
			glColor4ubv( (GLubyte *)&s_nodeId );
		}

		drawSphere( Vector3(), nif->get<float>( iShape, "Radius" ) );
	} else if ( name == "bhkMultiSphereShape" ) {
		if ( Node::SELECTING ) {
			int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iShape ) );
			glColor4ubv( (GLubyte *)&s_nodeId );
		}

		QModelIndex iSpheres = nif->getIndex( iShape, "Spheres" );

		for ( int r = 0; r < nif->rowCount( iSpheres ); r++ ) {
			drawSphere( nif->get<Vector3>( QModelIndex_child( iSpheres, r ), "Center" ), nif->get<float>( QModelIndex_child( iSpheres, r ), "Radius" ) );
		}
	} else if ( name == "bhkBoxShape" ) {
		if ( Node::SELECTING ) {
			int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iShape ) );
			glColor4ubv( (GLubyte *)&s_nodeId );
		}

		Vector3 v = nif->get<Vector3>( iShape, "Dimensions" );
		drawBox( v, -v );
	} else if ( name == "bhkCapsuleShape" ) {
		if ( Node::SELECTING ) {
			int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iShape ) );
			glColor4ubv( (GLubyte *)&s_nodeId );
		}

		drawCapsule( nif->get<Vector3>( iShape, "First Point" ), nif->get<Vector3>( iShape, "Second Point" ), nif->get<float>( iShape, "Radius" ) );
	} else if ( name == "bhkNiTriStripsShape" ) {
		glPushMatrix();
		float s = bhkInvScale( nif );
		glScalef( s, s, s );

		if ( Node::SELECTING ) {
			int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iShape ) );
			glColor4ubv( (GLubyte *)&s_nodeId );
		}

		drawNiTSS( nif, iShape );

		//if ( Options::getHavokState() == HAVOK_SOLID ) {
		//	QColor c = Options::hlColor();
		//	c.setAlphaF( 0.3 );
		//	glColor( Color4( c ) );
		//
		//	drawNiTSS( nif, iShape, true );
		//}

		glPopMatrix();
	} else if ( name == "bhkConvexVerticesShape" ) {
		if ( Node::SELECTING ) {
			int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iShape ) );
			glColor4ubv( (GLubyte *)&s_nodeId );
		}

		drawConvexHull( nif, iShape, 1.0 );

		//if ( Options::getHavokState() == HAVOK_SOLID ) {
		//	QColor c = Options::hlColor();
		//	c.setAlphaF( 0.3 );
		//	glColor( Color4( c ) );
		//
		//	drawConvexHull( nif, iShape, havokScale, true );
		//}

	} else if ( name == "bhkMoppBvTreeShape" ) {
		if ( !Node::SELECTING ) {
			if ( scene->currentBlock == nif->getBlockIndex( nif->getLink( iShape, "Shape" ) ) ) {
				// fix: add selected visual to havok meshes
				glHighlightColor();
				glLineWidth( GLView::Settings::lineWidthWireframe );	// taken from "DrawTriangleSelection"
			} else {
				glLineWidth( GLView::Settings::lineWidthWireframe * 0.625f );
				glColor3fv( origin_color3fv );
			}
		}

		drawHvkShape( nif, nif->getBlockIndex( nif->getLink( iShape, "Shape" ) ), stack, scene, origin_color3fv );
	} else if ( name == "bhkPackedNiTriStripsShape" || name == "hkPackedNiTriStripsData" ) {
		if ( Node::SELECTING ) {
			int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iShape ) );
			glColor4ubv( (GLubyte *)&s_nodeId );
		}

		QModelIndex iData = nif->getBlockIndex( nif->getLink( iShape, "Data" ) );

		if ( iData.isValid() ) {
			QVector<Vector3> verts = nif->getArray<Vector3>( iData, "Vertices" );
			QModelIndex iTris = nif->getIndex( iData, "Triangles" );

			for ( int t = 0; t < nif->rowCount( iTris ); t++ ) {
				Triangle tri = nif->get<Triangle>( QModelIndex_child( iTris, t ), "Triangle" );

				if ( tri[0] != tri[1] || tri[1] != tri[2] || tri[2] != tri[0] ) {
					glBegin( GL_LINE_STRIP );
					glVertex( verts.value( tri[0] ) );
					glVertex( verts.value( tri[1] ) );
					glVertex( verts.value( tri[2] ) );
					glVertex( verts.value( tri[0] ) );
					glEnd();
				}
			}

			// Handle Selection of hkPackedNiTriStripsData
			if ( scene->currentBlock == iData ) {
				int i = -1;
				QString n = scene->currentIndex.data( NifSkopeDisplayRole ).toString();
				QModelIndex iParent = scene->currentIndex.parent();

				if ( iParent.isValid() && iParent != iData ) {
					n = iParent.data( NifSkopeDisplayRole ).toString();
					i = scene->currentIndex.row();
				}

				if ( n == "Vertices" || n == "Normals" || n == "Vertex Colors" || n == "UV Sets" ) {
					DrawVertexSelection( verts, i );
				} else if ( ( n == "Faces" || n == "Triangles" ) ) {
					if ( i == -1 ) {
						glDepthFunc( GL_ALWAYS );
						glHighlightColor();

						//for ( int t = 0; t < nif->rowCount( iTris ); t++ )
						//	DrawTriangleIndex( verts, nif->get<Triangle>( QModelIndex_child( iTris, t ), "Triangle" ), t );
					} else if ( nif->isCompound( nif->itemStrType( scene->currentIndex ) ) ) {
						Triangle tri = nif->get<Triangle>( QModelIndex_child( iTris, i ), "Triangle" );
						DrawTriangleSelection( verts, tri );
						//DrawTriangleIndex( verts, tri, i );
					} else if ( nif->itemName( scene->currentIndex ) == "Normal" ) {
						Triangle tri = nif->get<Triangle>( scene->currentIndex.parent(), "Triangle" );
						Vector3 triCentre = ( verts.value( tri.v1() ) + verts.value( tri.v2() ) + verts.value( tri.v3() ) ) /  3.0;
						glLineWidth( GLView::Settings::lineWidthWireframe );
						glDepthFunc( GL_ALWAYS );
						glHighlightColor();
						glBegin( GL_LINES );
						glVertex( triCentre );
						glVertex( triCentre + nif->get<Vector3>( scene->currentIndex ) );
						glEnd();
					}
				} else if ( n == "Sub Shapes" ) {
					int start_vertex = 0;
					int end_vertex = 0;
					int num_vertices = nif->get<int>( scene->currentIndex, "Num Vertices" );

					int totalVerts = 0;
					if ( num_vertices > 0 ) {
						QModelIndex iParent = scene->currentIndex.parent();
						for ( int j = 0; j < i; j++ ) {
							totalVerts += nif->get<int>( QModelIndex_child( iParent, j ), "Num Vertices" );
						}

						end_vertex += totalVerts + num_vertices;
						start_vertex += totalVerts;
					}

					for ( int t = 0; t < nif->rowCount( iTris ); t++ ) {
						Triangle tri = nif->get<Triangle>( QModelIndex_child( iTris, t ), "Triangle" );

						if ( (start_vertex <= tri[0]) && (tri[0] < end_vertex) ) {
							if ( (start_vertex <= tri[1]) && (tri[1] < end_vertex) && (start_vertex <= tri[2]) && (tri[2] < end_vertex) ) {
								DrawTriangleSelection( verts, tri );
								//DrawTriangleIndex( verts, tri, t );
							} else {
								qDebug() << "triangle with multiple materials?" << t;
							}
						}
					}
				}
			}
			// Handle Selection of bhkPackedNiTriStripsShape
			else if ( scene->currentBlock == iShape ) {
				QString n = scene->currentIndex.data( NifSkopeDisplayRole ).toString();
				QModelIndex iParent = scene->currentIndex.parent();

				if ( iParent.isValid() && iParent != iShape ) {
					n = iParent.data( NifSkopeDisplayRole ).toString();
				}

				//qDebug() << n;
				// n == "Sub Shapes" if the array is selected and if an element of the array is selected
				// iParent != iShape only for the elements of the array
				if ( ( n == "Sub Shapes" ) && ( iParent != iShape ) ) {
					// get subshape vertex indices
					QModelIndex iSubShapes = iParent;
					QModelIndex iSubShape  = scene->currentIndex;
					int start_vertex = 0;
					int end_vertex = 0;

					for ( int subshape = 0; subshape < nif->rowCount( iSubShapes ); subshape++ ) {
						QModelIndex iCurrentSubShape = QModelIndex_child( iSubShapes, subshape );
						int num_vertices = nif->get<int>( iCurrentSubShape, "Num Vertices" );
						//qDebug() << num_vertices;
						end_vertex += num_vertices;

						if ( iCurrentSubShape == iSubShape ) {
							break;
						} else {
							start_vertex += num_vertices;
						}
					}

					// highlight the triangles of the subshape
					for ( int t = 0; t < nif->rowCount( iTris ); t++ ) {
						Triangle tri = nif->get<Triangle>( QModelIndex_child( iTris, t ), "Triangle" );

						if ( (start_vertex <= tri[0]) && (tri[0] < end_vertex) ) {
							if ( (start_vertex <= tri[1]) && (tri[1] < end_vertex) && (start_vertex <= tri[2]) && (tri[2] < end_vertex) ) {
								DrawTriangleSelection( verts, tri );
								//DrawTriangleIndex( verts, tri, t );
							} else {
								qDebug() << "triangle with multiple materials?" << t;
							}
						}
					}
				}
			}
		}
	} else if ( name == "bhkCompressedMeshShape" ) {
		if ( Node::SELECTING ) {
			int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iShape ) );
			glColor4ubv( (GLubyte *)&s_nodeId );
		}

		drawCMS( nif, iShape );

		//if ( Options::getHavokState() == HAVOK_SOLID ) {
		//	QColor c = Options::hlColor();
		//	c.setAlphaF( 0.3 );
		//	glColor( Color4( c ) );
		//
		//	drawCMS( nif, iShape, true );
		//}
	}

	stack.pop();
}

void drawHvkConstraint( const NifModel * nif, const QModelIndex & iConstraint, const Scene * scene )
{
	if ( !( nif && iConstraint.isValid() && scene && scene->hasOption(Scene::ShowConstraints) ) )
		return;

	if ( !scene->isSelModeObject() )
		return;

	Transform tBodyA;
	Transform tBodyB;

	auto iEntityA = bhkGetEntity( nif, iConstraint, "Entity A" );
	auto iEntityB = bhkGetEntity( nif, iConstraint, "Entity B" );
	if ( !iEntityA.isValid() || !iEntityB.isValid() )
		return;

	auto linkA = nif->getLink( iEntityA );
	auto linkB = nif->getLink( iEntityB );
	if ( !scene->bhkBodyTrans.contains( linkA ) || !scene->bhkBodyTrans.contains( linkB ) )
		return;

	tBodyA = scene->bhkBodyTrans.value( linkA );
	tBodyB = scene->bhkBodyTrans.value( linkB );

	auto hkFactor = bhkScaleMult( nif );
	auto hkFactorInv = 1.0 / hkFactor;

	tBodyA.scale = tBodyA.scale * hkFactorInv;
	tBodyB.scale = tBodyB.scale * hkFactorInv;

	Color3 color_a( 0.8f, 0.6f, 0.0f );
	Color3 color_b( 0.6f, 0.8f, 0.0f );

	if ( Node::SELECTING ) {
		int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iConstraint ) );
		glColor4ubv( (GLubyte *)&s_nodeId );
		glLineWidth( GLView::Settings::lineWidthSelect );	// make hitting a line a litlle bit more easy
	} else {
		if ( scene->currentBlock == nif->getBlockIndex( iConstraint ) ) {
			// fix: add selected visual to havok meshes
			glHighlightColor();
			color_a.fromQColor( highlightColor );
			color_b.setRGB( highlightColor.blueF(), highlightColor.redF(), highlightColor.greenF() );
		}
	}

	glPushMatrix();
	glLoadMatrix( scene->view );

	glPushAttrib( GL_ENABLE_BIT );
	glEnable( GL_DEPTH_TEST );

	QString name = nif->itemName( iConstraint );

	QModelIndex iConstraintInfo;

	if ( name == "bhkMalleableConstraint" || name == "bhkBreakableConstraint" ) {
		if ( nif->getIndex( iConstraint, "Ragdoll" ).isValid() ) {
			name = "bhkRagdollConstraint";
			iConstraintInfo = nif->getIndex( iConstraint, "Ragdoll" );
		} else if ( nif->getIndex( iConstraint, "Limited Hinge" ).isValid() ) {
			name = "bhkLimitedHingeConstraint";
			iConstraintInfo = nif->getIndex( iConstraint, "Limited Hinge" );
		} else if ( nif->getIndex( iConstraint, "Hinge" ).isValid() ) {
			name = "bhkHingeConstraint";
			iConstraintInfo = nif->getIndex( iConstraint, "Hinge" );
		} else if ( nif->getIndex( iConstraint, "Stiff Spring" ).isValid() ) {
			name = "bhkStiffSpringConstraint";
			iConstraintInfo = nif->getIndex( iConstraint, "Stiff Spring" );
		}
	} else {
		iConstraintInfo = nif->getIndex( iConstraint, "Constraint" );
		if ( !iConstraintInfo.isValid() )
			iConstraintInfo = iConstraint;
	}

	Vector3 pivotA( nif->get<Vector4>( iConstraintInfo, "Pivot A" ) * hkFactor );
	Vector3 pivotB( nif->get<Vector4>( iConstraintInfo, "Pivot B" ) * hkFactor );

	if ( name == "bhkLimitedHingeConstraint" ) {
		const Vector3 axisA( nif->get<Vector4>( iConstraintInfo, "Axis A" ) );
		const Vector3 axisA1( nif->get<Vector4>( iConstraintInfo, "Perp Axis In A1" ) );
		const Vector3 axisA2( nif->get<Vector4>( iConstraintInfo, "Perp Axis In A2" ) );

		const Vector3 axisB( nif->get<Vector4>( iConstraintInfo, "Axis B" ) );
		const Vector3 axisB2( nif->get<Vector4>( iConstraintInfo, "Perp Axis In B2" ) );

		const float minAngle = nif->get<float>( iConstraintInfo, "Min Angle" );
		const float maxAngle = nif->get<float>( iConstraintInfo, "Max Angle" );

		glPushMatrix();
		glMultMatrix( tBodyA );

		if ( !Node::SELECTING )
			glColor( color_a );

		glBegin( GL_POINTS ); glVertex( pivotA ); glEnd();
		glBegin( GL_LINES ); glVertex( pivotA ); glVertex( pivotA + axisA ); glEnd();
		drawDashLine( pivotA, pivotA + axisA1, 14 );
		drawDashLine( pivotA, pivotA + axisA2, 14 );
		drawCircle( pivotA, axisA, 1.0 );
		drawSolidArc( pivotA, axisA / 5, axisA2, axisA1, minAngle, maxAngle, 1.0f );
		glPopMatrix();

		glPushMatrix();
		glMultMatrix( tBodyB );

		if ( !Node::SELECTING )
			glColor( color_b );

		glBegin( GL_POINTS ); glVertex( pivotB ); glEnd();
		glBegin( GL_LINES ); glVertex( pivotB ); glVertex( pivotB + axisB ); glEnd();
		drawDashLine( pivotB + axisB2, pivotB, 14 );
		drawDashLine( pivotB + Vector3::crossproduct( axisB2, axisB ), pivotB, 14 );
		drawCircle( pivotB, axisB, 1.01f );
		drawSolidArc( pivotB, axisB / 7, axisB2, Vector3::crossproduct( axisB2, axisB ), minAngle, maxAngle, 1.01f );
		glPopMatrix();

		glMultMatrix( tBodyA );
		float angle = Vector3::angle( tBodyA.rotation * axisA2, tBodyB.rotation * axisB2 );

		if ( !Node::SELECTING )
			glColor( color_a );

		glBegin( GL_LINES );
		glVertex( pivotA );
		glVertex( pivotA + axisA1 * cosf( angle ) + axisA2 * sinf( angle ) );
		glEnd();
	} else if ( name == "bhkHingeConstraint" ) {
		const Vector3 axisA1( nif->get<Vector4>( iConstraintInfo, "Perp Axis In A1" ) );
		const Vector3 axisA2( nif->get<Vector4>( iConstraintInfo, "Perp Axis In A2" ) );
		const Vector3 axisA( Vector3::crossproduct( axisA1, axisA2 ) );

		const Vector3 axisB( nif->get<Vector4>( iConstraintInfo, "Axis B" ) );

		const Vector3 axisB1( axisB[1], axisB[2], axisB[0] );
		const Vector3 axisB2( Vector3::crossproduct( axisB, axisB1 ) );

		/*
		 * This should be correct but is visually strange...
		 *
		Vector3 axisB1temp;
		Vector3 axisB2temp;

		if ( nif->checkVersion( 0, 0x14000002 ) )
		{
		    Vector3 axisB1temp( axisB[1], axisB[2], axisB[0] );
		    Vector3 axisB2temp( Vector3::crossproduct( axisB, axisB1temp ) );
		}
		else if ( nif->checkVersion( 0x14020007, 0 ) )
		{
		    Vector3 axisB1temp( nif->get<Vector4>( iConstraintInfo, "Perp Axis In B1" ) );
		    Vector3 axisB2temp( nif->get<Vector4>( iConstraintInfo, "Perp Axis In B2" ) );
		}

		const Vector3 axisB1( axisB1temp );
		const Vector3 axisB2( axisB2temp );
		*/

		const float minAngle = (float)-PI;
		const float maxAngle = (float)+PI;

		glPushMatrix();
		glMultMatrix( tBodyA );

		if ( !Node::SELECTING )
			glColor( color_a );

		glBegin( GL_POINTS ); glVertex( pivotA ); glEnd();
		drawDashLine( pivotA, pivotA + axisA1 );
		drawDashLine( pivotA, pivotA + axisA2 );
		drawSolidArc( pivotA, axisA / 5, axisA2, axisA1, minAngle, maxAngle, 1.0f, 16 );
		glPopMatrix();

		glMultMatrix( tBodyB );

		if ( !Node::SELECTING )
			glColor( color_b );

		glBegin( GL_POINTS ); glVertex( pivotB ); glEnd();
		glBegin( GL_LINES ); glVertex( pivotB ); glVertex( pivotB + axisB ); glEnd();
		drawSolidArc( pivotB, axisB / 7, axisB2, axisB1, minAngle, maxAngle, 1.01f, 16 );
	} else if ( name == "bhkStiffSpringConstraint" ) {
		const float length = nif->get<float>( iConstraintInfo, "Length" );

		if ( !Node::SELECTING )
			glColor( color_b );

		drawSpring( pivotA, pivotB, length );
	} else if ( name == "bhkRagdollConstraint" ) {
		const Vector3 planeA( nif->get<Vector4>( iConstraintInfo, "Plane A" ) );
		const Vector3 planeB( nif->get<Vector4>( iConstraintInfo, "Plane B" ) );

		const Vector3 twistA( nif->get<Vector4>( iConstraintInfo, "Twist A" ) );
		const Vector3 twistB( nif->get<Vector4>( iConstraintInfo, "Twist B" ) );

		const float coneAngle( nif->get<float>( iConstraintInfo, "Cone Max Angle" ) );

		const float minPlaneAngle( nif->get<float>( iConstraintInfo, "Plane Min Angle" ) );
		const float maxPlaneAngle( nif->get<float>( iConstraintInfo, "Plane Max Angle" ) );

		// Unused? GCC complains
		/*
		const float minTwistAngle( nif->get<float>( iConstraintInfo, "Twist Min Angle" ) );
		const float maxTwistAngle( nif->get<float>( iConstraintInfo, "Twist Max Angle" ) );
		*/

		glPushMatrix();
		glMultMatrix( tBodyA );

		if ( !Node::SELECTING )
			glColor( color_a );

		glPopMatrix();

		glPushMatrix();
		glMultMatrix( tBodyA );

		if ( !Node::SELECTING )
			glColor( color_a );

		glBegin( GL_POINTS ); glVertex( pivotA ); glEnd();
		glBegin( GL_LINES ); glVertex( pivotA ); glVertex( pivotA + twistA ); glEnd();
		drawDashLine( pivotA, pivotA + planeA, 14 );
		drawRagdollCone( pivotA, twistA, planeA, coneAngle, minPlaneAngle, maxPlaneAngle );
		glPopMatrix();

		glPushMatrix();
		glMultMatrix( tBodyB );

		if ( !Node::SELECTING )
			glColor( color_b );

		glBegin( GL_POINTS ); glVertex( pivotB ); glEnd();
		glBegin( GL_LINES ); glVertex( pivotB ); glVertex( pivotB + twistB ); glEnd();
		drawDashLine( pivotB + planeB, pivotB, 14 );
		drawRagdollCone( pivotB, twistB, planeB, coneAngle, minPlaneAngle, maxPlaneAngle );
		glPopMatrix();
	} else if ( name == "bhkPrismaticConstraint" ) {
		const Vector3 planeNormal( nif->get<Vector4>( iConstraintInfo, "Plane A" ) );
		const Vector3 slidingAxis( nif->get<Vector4>( iConstraintInfo, "Sliding A" ) );

		const float minDistance = nif->get<float>( iConstraintInfo, "Min Distance" );
		const float maxDistance = nif->get<float>( iConstraintInfo, "Max Distance" );

		const Vector3 d1 = pivotA + slidingAxis * minDistance;
		const Vector3 d2 = pivotA + slidingAxis * maxDistance;

		/* draw Pivot A and Plane */
		glPushMatrix();
		glMultMatrix( tBodyA );

		if ( !Node::SELECTING )
			glColor( color_a );

		glBegin( GL_POINTS ); glVertex( pivotA ); glEnd();
		glBegin( GL_LINES ); glVertex( pivotA ); glVertex( pivotA + planeNormal ); glEnd();
		drawDashLine( pivotA, d1, 14 );

		/* draw rail */
		if ( minDistance < maxDistance ) {
			drawRail( d1, d2 );
		}

		/*draw first marker*/
		Transform t;
		float angle = atan2f( slidingAxis[1], slidingAxis[0] );

		if ( slidingAxis[0] < 0.0001f && slidingAxis[1] < 0.0001f ) {
			angle = float(HALF_PI);
		}

		t.translation = d1;
		t.rotation.fromEuler( 0.0f, 0.0f, angle );
		glMultMatrix( t );

		angle = -asinf( slidingAxis[2] / slidingAxis.length() );
		t.translation = Vector3( 0.0f, 0.0f, 0.0f );
		t.rotation.fromEuler( 0.0f, angle, 0.0f );
		glMultMatrix( t );

		drawMarker( &BumperMarker01 );

		/*draw second marker*/
		t.translation = Vector3( minDistance < maxDistance ? ( d2 - d1 ).length() : 0.0f, 0.0f, 0.0f );
		t.rotation.fromEuler( 0.0f, 0.0f, (float)PI );
		glMultMatrix( t );

		drawMarker( &BumperMarker01 );
		glPopMatrix();

		/* draw Pivot B */
		glPushMatrix();
		glMultMatrix( tBodyB );

		if ( !Node::SELECTING )
			glColor( color_b );

		glBegin( GL_POINTS ); glVertex( pivotB ); glEnd();
		glPopMatrix();
	}

	glPopAttrib();
	glPopMatrix();
}

void Node::drawHavok()
{
	if ( !scene->isSelModeObject() )
		return;

	// TODO: Why are all these here - "drawNodes", "drawFurn", "drawHavok"?
	// Idea: Make them go to their own classes in different cpp files
	for ( Node * node : children.list() ) {
		node->drawHavok();
	}

	auto nif = NifModel::fromValidIndex(iBlock);
	if ( !nif )
		return;

	//Check if there's any old style collision bounding box set
	if ( nif->get<bool>( iBlock, "Has Bounding Box" ) == true ) {
		QModelIndex iBox = nif->getIndex( iBlock, "Bounding Box" );

		Transform bt;

		bt.translation = nif->get<Vector3>( iBox, "Translation" );
		bt.rotation = nif->get<Matrix>( iBox, "Rotation" );
		bt.scale = 1.0f;

		Vector3 rad = nif->get<Vector3>( iBox, "Radius" );

		glPushMatrix();
		glLoadMatrix( scene->view );
		// The Morrowind construction set seems to completely ignore the node transform
		//glMultMatrix( worldTrans() );
		glMultMatrix( bt );

		if ( Node::SELECTING ) {
			int s_nodeId = ID2COLORKEY( nodeId );
			glColor4ubv( (GLubyte *)&s_nodeId );
		} else {
			glColor( Color3( 1.0f, 0.0f, 0.0f ) );
			glDisable( GL_LIGHTING );
		}

		glLineWidth( GLView::Settings::lineWidthWireframe * 0.625f );
		drawBox( rad, -rad );

		glPopMatrix();
	}

	// Only Bethesda support after this
	if ( nif->getBSVersion() == 0 )
		return;

	// Draw BSMultiBound
	auto iBSMultiBound = nif->getBlockIndex( nif->getLink( iBlock, "Multi Bound" ), "BSMultiBound" );
	if ( iBSMultiBound.isValid() ) {

		auto iBSMultiBoundData = nif->getBlockIndex( nif->getLink( iBSMultiBound, "Data" ), "BSMultiBoundData" );
		if ( iBSMultiBoundData.isValid() ) {

			Vector3 a, b;

			glPushMatrix();
			glLoadMatrix( scene->view );
			glMultMatrix( worldTrans() );

			// BSMultiBoundAABB
			if ( nif->isNiBlock( iBSMultiBoundData, "BSMultiBoundAABB" ) ) {
				auto pos = nif->get<Vector3>( iBSMultiBoundData, "Position" );
				auto extent = nif->get<Vector3>( iBSMultiBoundData, "Extent" );

				a = pos + extent;
				b = pos - extent;
			}

			// BSMultiBoundOBB
			if ( nif->isNiBlock( iBSMultiBoundData, "BSMultiBoundOBB" ) ) {
				auto center = nif->get<Vector3>( iBSMultiBoundData, "Center" );
				auto size = nif->get<Vector3>( iBSMultiBoundData, "Size" );
				auto matrix = nif->get<Matrix>( iBSMultiBoundData, "Rotation" );

				a = size;
				b = -size;

				Transform t;
				t.rotation = matrix;
				t.translation = center;
				glMultMatrix( t );
			}

			if ( Node::SELECTING ) {
				int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iBSMultiBoundData ) );
				glColor4ubv( (GLubyte *)&s_nodeId );
				glLineWidth( GLView::Settings::lineWidthSelect );
			} else {
				glColor( Color4( 1.0f, 1.0f, 1.0f, 0.6f ) );
				glDisable( GL_LIGHTING );
				glLineWidth( GLView::Settings::lineWidthWireframe * 0.625f );
			}

			drawBox( a, b );
			glPopMatrix();
		}
	}

	// Draw BSBound dimensions
	QModelIndex iExtraDataList = nif->getIndex( iBlock, "Extra Data List" );

	if ( iExtraDataList.isValid() ) {
		for ( int d = 0; d < nif->rowCount( iExtraDataList ); d++ ) {
			QModelIndex iBound = nif->getBlockIndex( nif->getLink( QModelIndex_child( iExtraDataList, d ) ), "BSBound" );

			if ( !iBound.isValid() )
				continue;

			Vector3 center = nif->get<Vector3>( iBound, "Center" );
			Vector3 dim = nif->get<Vector3>( iBound, "Dimensions" );

			glPushMatrix();
			glLoadMatrix( scene->view );
			// Not sure if world transform is taken into account
			glMultMatrix( worldTrans() );

			if ( Node::SELECTING ) {
				int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iBound ) );
				glColor4ubv( (GLubyte *)&s_nodeId );
			} else {
				glColor( Color3( 1.0f, 0.0f, 0.0f ) );
				glDisable( GL_LIGHTING );
			}

			glLineWidth( GLView::Settings::lineWidthWireframe * 0.625f );
			drawBox( dim + center, -dim + center );

			glPopMatrix();
		}
	}

	QModelIndex iObject = nif->getBlockIndex( nif->getLink( iBlock, "Collision Object" ) );
	if ( !iObject.isValid() )
		return;

	QModelIndex iBody = nif->getBlockIndex( nif->getLink( iObject, "Body" ) );

	glPushMatrix();
	glLoadMatrix( scene->view );
	glMultMatrix( scene->bhkBodyTrans.value( nif->getBlockNumber( iBody ) ) );


	//qDebug() << "draw obj" << nif->getBlockNumber( iObject ) << nif->itemName( iObject );

	if ( !Node::SELECTING ) {
		glEnable( GL_DEPTH_TEST );
		glDepthMask( GL_TRUE );
		glDepthFunc( GL_LEQUAL );
		glDisable( GL_TEXTURE_2D );
		glDisable( GL_NORMALIZE );
		glDisable( GL_LIGHTING );
		glDisable( GL_COLOR_MATERIAL );
		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glDisable( GL_ALPHA_TEST );
	}

	glPointSize( GLView::Settings::vertexPointSize );
	glLineWidth( GLView::Settings::lineWidthWireframe );

	static const float colors[8][3] = {
		{ 0.0f, 1.0f, 0.0f },
		{ 1.0f, 0.0f, 0.0f },
		{ 1.0f, 0.0f, 1.0f },
		{ 1.0f, 1.0f, 1.0f },
		{ 0.5f, 0.5f, 1.0f },
		{ 1.0f, 0.8f, 0.0f },
		{ 1.0f, 0.8f, 0.4f },
		{ 0.0f, 1.0f, 1.0f }
	};

	int color_index = nif->get<int>( iBody, "Layer" ) & 7;
	glColor3fv( colors[ color_index ] );

	if ( !Node::SELECTING ) {
		if ( scene->currentBlock == nif->getBlockIndex( nif->getLink( iBody, "Shape" ) ) ) {
			// fix: add selected visual to havok meshes
			glHighlightColor(); // TODO: idea: I do not recommend mimicking the Open GL API
			                    // It confuses the one who reads the code. And the Open GL API is
			                    // in constant development.
			glLineWidth( GLView::Settings::lineWidthHighlight );
			//glPointSize( GLView::Settings::vertexSelectPointSize );
		}
	}

	QStack<QModelIndex> shapeStack;

	if ( Node::SELECTING )
		glLineWidth( GLView::Settings::lineWidthSelect );	// make selection click a little more easy

	drawHvkShape( nif, nif->getBlockIndex( nif->getLink( iBody, "Shape" ) ), shapeStack, scene, colors[ color_index ] );

	if ( Node::SELECTING && scene->hasOption(Scene::ShowAxes) ) {
		int s_nodeId = ID2COLORKEY( nif->getBlockNumber( iBody ) );
		glColor4ubv( (GLubyte *)&s_nodeId );
		glDepthFunc( GL_ALWAYS );
		drawAxes( Vector3( nif->get<Vector4>( iBody, "Center" ) ), 1.0f / bhkScaleMult( nif ), false );
		glDepthFunc( GL_LEQUAL );
	} else if ( scene->hasOption(Scene::ShowAxes) ) {
		drawAxes( Vector3( nif->get<Vector4>( iBody, "Center" ) ), 1.0f / bhkScaleMult( nif ) );
	}

	glPopMatrix();

	for ( const auto l : nif->getLinkArray( iBody, "Constraints" ) ) {
		QModelIndex iConstraint = nif->getBlockIndex( l );

		if ( nif->blockInherits( iConstraint, "bhkConstraint" ) )
			drawHvkConstraint( nif, iConstraint, scene );
	}
}

void drawFurnitureMarker( const NifModel * nif, const QModelIndex & iPosition )
{
	Vector3 offs = nif->get<Vector3>( iPosition, "Offset" );
	quint16 orient = nif->get<quint16>( iPosition, "Orientation" );
	quint8 ref1 = nif->get<quint8>( iPosition, "Position Ref 1" );
	quint8 ref2 = nif->get<quint8>( iPosition, "Position Ref 2" );

	const GLMarker * mark[5];
	Vector3 flip[5];
	Vector3 pos( 1, 1, 1 );
	Vector3 neg( -1, 1, 1 );

	float xOffset = 0.0f;
	float zOffset = 0.0f;
	float yOffset = 0.0f;
	float roll;

	int i = 0;

	if ( ref1 == 0 ) {
		float heading = nif->get<float>( iPosition, "Heading" );
		quint16 type = nif->get<quint16>( iPosition, "Animation Type" );
		int entry = nif->get<int>( iPosition, "Entry Properties" );

		if ( type == 0 ) return;

		// Sit=1, Sleep=2, Lean=3
		// Front=1, Behind=2, Right=4, Left=8, Up=16(0x10)

		switch ( type ) {
		case 1:
			// Sit Type

			zOffset = -34.00f;

			if ( entry & 0x1 ) {
				// Chair Front
				flip[i] = pos;
				mark[i] = &ChairFront;
				i++;
			}
			if ( entry & 0x2 ) {
				// Chair Behind
				flip[i] = pos;
				mark[i] = &ChairBehind;
				i++;
			}
			if ( entry & 0x4 ) {
				// Chair Right
				flip[i] = neg;
				mark[i] = &ChairLeft;
				i++;
			}
			if ( entry & 0x8 ) {
				// Chair Left
				flip[i] = pos;
				mark[i] = &ChairLeft;
				i++;
			}
			break;
		case 2:
			// Sleep Type

			zOffset = -34.00f;

			if ( entry & 0x1 ) {
				// Bed Front
				//flip[i] = pos;
				//mark[i] = &FurnitureMarker03;
				//i++;
			}
			if ( entry & 0x2 ) {
				// Bed Behind
				//flip[i] = pos;
				//mark[i] = &FurnitureMarker04;
				//i++;
			}
			if ( entry & 0x4 ) {
				// Bed Right
				flip[i] = neg;
				mark[i] = &BedLeft;
				i++;
			}
			if ( entry & 0x8 ) {
				// Bed Left
				flip[i] = pos;
				mark[i] = &BedLeft;
				i++;
			}
			if ( entry & 0x10 ) {
				// Bed Up????
				// This is sometimes used as a real bed position
				// Other times it is a dummy
				flip[i] = neg;
				mark[i] = &BedLeft;
				i++;
			}
			break;
		case 3:
			break;
		default:
			break;
		}

		roll = heading;
	} else {
		if ( ref1 != ref2 ) {
			qDebug() << "Position Ref 1 and 2 are not equal";
			return;
		}

		switch ( ref1 ) {
		case 1:
			mark[0] = &FurnitureMarker01; // Single Bed
			break;

		case 2:
			flip[0] = neg;
			mark[0] = &FurnitureMarker01;
			break;

		case 3:
			mark[0] = &FurnitureMarker03; // Ground Bed?
			break;

		case 4:
			mark[0] = &FurnitureMarker04; // Ground Bed? Behind
			break;

		case 11:
			mark[0] = &FurnitureMarker11; // Chair Left
			break;

		case 12:
			flip[0] = neg;
			mark[0] = &FurnitureMarker11;
			break;

		case 13:
			mark[0] = &FurnitureMarker13; // Chair Behind
			break;

		case 14:
			mark[0] = &FurnitureMarker14; // Chair Front
			break;

		default:
			qDebug() << "Unknown furniture marker " << ref1;
			return;
		}

		i = 1;

		// TODO: FIX: This makes no sense
		roll = float( orient ) / 6284.0 * 2.0 * (-M_PI);
	}

	if ( Node::SELECTING ) {
		GLint id = ( nif->getBlockNumber( iPosition ) & 0xffff ) | ( ( iPosition.row() & 0xffff ) << 16 );
		int s_nodeId = ID2COLORKEY( id );
		glColor4ubv( (GLubyte *)&s_nodeId );
	}

	for ( int n = 0; n < i; n++ ) {
		glPushMatrix();

		Transform t;
		t.rotation.fromEuler( 0, 0, roll );
		t.translation = offs;
		t.translation[0] += xOffset;
		t.translation[1] += yOffset;
		t.translation[2] += zOffset;

		glMultMatrix( t );

		glScale( flip[n] );

		drawMarker( mark[n] );

		glPopMatrix();
	}
}

void Node::drawFurn()
{
	for ( Node * node : children.list() ) {
		node->drawFurn();
	}

	auto nif = NifModel::fromValidIndex(iBlock);
	if ( !nif )
		return;

	if ( !scene->isSelModeObject() )
		return;

	QModelIndex iExtraDataList = nif->getIndex( iBlock, "Extra Data List" );

	if ( !iExtraDataList.isValid() )
		return;

	if ( !Node::SELECTING ) {
		glEnable( GL_DEPTH_TEST );
		glDepthMask( GL_FALSE );
		glDepthFunc( GL_LEQUAL );
		glDisable( GL_TEXTURE_2D );
		glDisable( GL_NORMALIZE );
		glDisable( GL_LIGHTING );
		glDisable( GL_COLOR_MATERIAL );
		glDisable( GL_CULL_FACE );
		glDisable( GL_BLEND );
		glDisable( GL_ALPHA_TEST );
		glColor4f( 1, 1, 1, 1 );
		glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
	}

	glLineWidth( GLView::Settings::lineWidthWireframe * 0.625f );

	glPushMatrix();

	glMultMatrix( viewTrans() );

	for ( int p = 0; p < nif->rowCount( iExtraDataList ); p++ ) {
		// DONE: never seen Furn in nifs, so there may be a need of a fix here later - saw one, fixed a bug
		QModelIndex iFurnMark = nif->getBlockIndex( nif->getLink( QModelIndex_child( iExtraDataList, p ) ), "BSFurnitureMarker" );

		if ( !iFurnMark.isValid() )
			continue;

		QModelIndex iPositions = nif->getIndex( iFurnMark, "Positions" );

		if ( !iPositions.isValid() )
			break;

		for ( int j = 0; j < nif->rowCount( iPositions ); j++ ) {
			QModelIndex iPosition = QModelIndex_child( iPositions, j );

			if ( scene->currentIndex == iPosition )
				glHighlightColor();
			else
				glNormalColor();

			drawFurnitureMarker( nif, iPosition );
		}
	}

	glPopMatrix();
}

void Node::drawShapes( NodeList * secondPass )
{
	if ( isHidden() )
		return;

	if ( presorted )
		children.orderedNodeSort();

	for ( Node * node : children.list() )
		node->drawShapes( secondPass );
}

#define Farg( X ) arg( X, 0, 'f', 5 )

QString trans2string( Transform t )
{
	float xr, yr, zr;
	t.rotation.toEuler( xr, yr, zr );
	return QString( "translation  X %1, Y %2, Z %3\n" ).Farg( t.translation[0] ).Farg( t.translation[1] ).Farg( t.translation[2] )
	       +   QString( "rotation     Y %1, P %2, R %3  " ).Farg( rad2deg(xr) ).Farg( rad2deg(yr) ).Farg( rad2deg(zr) )
	       +   QString( "( (%1, %2, %3), " ).Farg( t.rotation( 0, 0 ) ).Farg( t.rotation( 0, 1 ) ).Farg( t.rotation( 0, 2 ) )
	       +   QString( "(%1, %2, %3), " ).Farg( t.rotation( 1, 0 ) ).Farg( t.rotation( 1, 1 ) ).Farg( t.rotation( 1, 2 ) )
	       +   QString( "(%1, %2, %3) )\n" ).Farg( t.rotation( 2, 0 ) ).Farg( t.rotation( 2, 1 ) ).Farg( t.rotation( 2, 2 ) )
	       +   QString( "scale        %1\n" ).Farg( t.scale );
}

QString Node::textStats() const
{
	return QString( "%1\n\nglobal\n%2\nlocal\n%3\n" ).arg( name, trans2string( worldTrans() ), trans2string( localTrans() ) );
}

BoundSphere Node::bounds() const
{
	BoundSphere boundsphere;

	// the node itself
	if ( scene->hasOption(Scene::ShowNodes) || scene->hasOption(Scene::ShowCollision) ) {
		boundsphere |= BoundSphere( worldTrans().translation, 0 );
	}

	auto nif = NifModel::fromValidIndex(iBlock);
	if ( !nif )
		return boundsphere;

	// old style collision bounding box
	if ( nif->get<bool>( iBlock, "Has Bounding Box" ) == true ) {
		QModelIndex iBox = nif->getIndex( iBlock, "Bounding Box" );
		Vector3 trans = nif->get<Vector3>( iBox, "Translation" );
		Vector3 rad = nif->get<Vector3>( iBox, "Radius" );
		boundsphere |= BoundSphere( trans, rad.length() );
	}

	if ( nif->itemStrType( iBlock ) == "NiMesh" )
		boundsphere |= BoundSphere( nif, iBlock );

	// BSBound collision bounding box
	QModelIndex iExtraDataList = nif->getIndex( iBlock, "Extra Data List" );

	if ( iExtraDataList.isValid() ) {
		for ( int d = 0; d < nif->rowCount( iExtraDataList ); d++ ) {
			QModelIndex iBound = nif->getBlockIndex( nif->getLink( QModelIndex_child( iExtraDataList, d ) ), "BSBound" );

			if ( !iBound.isValid() )
				continue;

			Vector3 center = nif->get<Vector3>( iBound, "Center" );
			Vector3 dim = nif->get<Vector3>( iBound, "Dimensions" );
			boundsphere |= BoundSphere( center, dim.length() );
		}
	}

	return boundsphere;
}


LODNode::LODNode( Scene * scene, const QModelIndex & iBlock )
	: Node( scene, iBlock )
{
}

void LODNode::clear()
{
	Node::clear();
	ranges.clear();
}

void LODNode::updateImpl( const NifModel * nif, const QModelIndex & index )
{
	Node::updateImpl( nif, index );

	if ( ( index == iBlock ) || ( iData.isValid() && index == iData ) ) {
		ranges.clear();
		iData = nif->getBlockIndex( nif->getLink( iBlock, "LOD Level Data" ), "NiRangeLODData" );
		QModelIndex iLevels;

		if ( iData.isValid() ) {
			center  = nif->get<Vector3>( iData, "LOD Center" );
			iLevels = nif->getIndex( iData, "LOD Levels" );
		} else {
			center  = nif->get<Vector3>( iBlock, "LOD Center" );
			iLevels = nif->getIndex( iBlock, "LOD Levels" );
		}

		if ( iLevels.isValid() ) {
			for ( int r = 0; r < nif->rowCount( iLevels ); r++ ) {
				ranges.append( { nif->get<float>( QModelIndex_child( iLevels, r ), "Near Extent" ),
				                 nif->get<float>( QModelIndex_child( iLevels, r ), "Far Extent" ) }
				);
			}
		}
	}
}

void LODNode::transform()
{
	Node::transform();

	if ( children.list().isEmpty() )
		return;

	if ( ranges.isEmpty() ) {
		for ( Node * child : children.list() ) {
			child->flags.node.hidden = true;
		}
		children.list().first()->flags.node.hidden = false;
		return;
	}

	float distance = ( viewTrans() * center ).length();

	int c = 0;
	for ( Node * child : children.list() ) {
		if ( c < ranges.count() )
			child->flags.node.hidden = !( ranges[c].first <= distance && distance < ranges[c].second );
		else
			child->flags.node.hidden = true;

		c++;
	}
}


BillboardNode::BillboardNode( Scene * scene, const QModelIndex & iBlock )
	: Node( scene, iBlock )
{
}

const Transform & BillboardNode::viewTrans() const
{
	if ( scene->viewTrans.contains( nodeId ) )
		return scene->viewTrans[ nodeId ];

	Transform t;

	if ( parent )
		t = parent->viewTrans() * local;
	else
		t = scene->view * worldTrans();

	t.rotation = Matrix();

	scene->viewTrans.insert( nodeId, t );
	return scene->viewTrans[ nodeId ];
}
