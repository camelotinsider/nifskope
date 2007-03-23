#include "../spellbook.h"

#include "../NvTriStrip/qtwrapper.h"

#include "blocks.h"

#include <QDebug>

class spLimitedHingeHelper : public Spell
{
public:
	QString name() const { return "A -> B"; }
	QString page() const { return "Havok"; }
	
	bool isApplicable( const NifModel * nif, const QModelIndex & index )
	{
		return nif && nif->isNiBlock( nif->getBlock( index ), "bhkLimitedHingeConstraint" );
	}
	
	QModelIndex cast( NifModel * nif, const QModelIndex & index )
	{
		QModelIndex iConstraint = nif->getBlock( index );
		
		QModelIndex iBodyA = nif->getBlock( nif->getLink( nif->getIndex( iConstraint, "Bodies" ).child( 0, 0 ) ), "bhkRigidBody" );
		QModelIndex iBodyB = nif->getBlock( nif->getLink( nif->getIndex( iConstraint, "Bodies" ).child( 1, 0 ) ), "bhkRigidBody" );
		
		if ( ! iBodyA.isValid() || ! iBodyB.isValid() )
		{
			qWarning() << "didn't find the bodies for this constraint";
			return index;
		}
		
		Transform transA = bodyTrans( nif, iBodyA );
		Transform transB = bodyTrans( nif, iBodyB );
		
		iConstraint = nif->getIndex( iConstraint, "Limited Hinge" );
		if ( ! iConstraint.isValid() )
			return index;
		
		Vector3 pivot = Vector3( nif->get<Vector4>( iConstraint, "Pivot A" ) ) * 7.0;
		pivot = transA * pivot;
		pivot = transB.rotation.inverted() * ( pivot - transB.translation ) / transB.scale / 7.0;
		nif->set<Vector4>( iConstraint, "Pivot B", Vector4( pivot[0], pivot[1], pivot[2], 0 ) );
		
		Vector3 axle = Vector3( nif->get<Vector4>( iConstraint, "Axle A" ) );
		axle = transA.rotation * axle;
		axle = transB.rotation.inverted() * axle;
		nif->set<Vector4>( iConstraint, "Axle B", Vector4( axle[0], axle[1], axle[2], 0 ) );
		
		axle = Vector3( nif->get<Vector4>( iConstraint, "Perp2AxleInA2" ) );
		axle = transA.rotation * axle;
		axle = transB.rotation.inverted() * axle;
		nif->set<Vector4>( iConstraint, "Perp2AxleInB2", Vector4( axle[0], axle[1], axle[2], 0 ) );
		
		return index;
	}
	
	static Transform bodyTrans( const NifModel * nif, const QModelIndex & index )
	{
		Transform t;
		if ( nif->isNiBlock( index, "bhkRigidBodyT" ) )
		{
			t.translation = nif->get<Vector3>( index, "Translation" ) * 7;
			t.rotation.fromQuat( nif->get<Quat>( index, "Rotation" ) );
		}
		
		qint32 l = nif->getBlockNumber( index );
		
		while ( ( l = nif->getParent( l ) ) >= 0 )
		{
			QModelIndex iAV = nif->getBlock( l, "NiAVObject" );
			if ( iAV.isValid() )
				t = Transform( nif, iAV ) * t;
		}
		
		return t;
	}
};

REGISTER_SPELL( spLimitedHingeHelper )

class spPackHavokStrips : public Spell
{
public:
	QString name() const { return tr( "Pack Strips" ); }
	QString page() const { return tr( "Havok" ); }
	
	bool isApplicable( const NifModel * nif, const QModelIndex & idx )
	{
		return nif->isNiBlock( idx, "bhkNiTriStripsShape" );
	}
	
	QModelIndex cast( NifModel * nif, const QModelIndex & iBlock )
	{
		QPersistentModelIndex iShape( iBlock );
		
		QVector<Vector3> vertices;
		QVector<Triangle> triangles;
		QVector<Vector3> normals;
		
		foreach ( qint32 lData, nif->getLinkArray( iShape, "Strips Data" ) )
		{
			QModelIndex iData = nif->getBlock( lData, "NiTriStripsData" );
			
			if ( iData.isValid() )
			{
				QVector<Vector3> vrts = nif->getArray<Vector3>( iData, "Vertices" );
				QVector<Triangle> tris;
				QVector<Vector3> nrms;
				
				QModelIndex iPoints = nif->getIndex( iData, "Points" );
				for ( int x = 0; x < nif->rowCount( iPoints ); x++ )
				{
					tris += triangulate( nif->getArray<quint16>( iPoints.child( x, 0 ) ) );
				}
				
				QMutableVectorIterator<Triangle> it( tris );
				while ( it.hasNext() )
				{
					Triangle & tri = it.next();
					
					Vector3 a = vrts.value( tri[0] );
					Vector3 b = vrts.value( tri[1] );
					Vector3 c = vrts.value( tri[2] );
					
					nrms << Vector3::crossproduct( b - a, c - a ).normalize();
					
					tri[0] += vertices.count();
					tri[1] += vertices.count();
					tri[2] += vertices.count();
				}
				
				foreach ( Vector3 v, vrts )
					vertices += v / 7;
				triangles += tris;
				normals += nrms;
			}
		}
		
		if ( vertices.isEmpty() || triangles.isEmpty() )
		{
			qWarning() << tr( "no mesh data was found" );
			return iShape;
		}
		
		QPersistentModelIndex iPackedShape = nif->insertNiBlock( "bhkPackedNiTriStripsShape", nif->getBlockNumber( iShape ) );
		
		nif->set<int>( iPackedShape, "Num Sub Shapes", 1 );
		QModelIndex iSubShapes = nif->getIndex( iPackedShape, "Sub Shapes" );
		nif->updateArray( iSubShapes );
		nif->set<int>( iSubShapes.child( 0, 0 ), "Layer", 1 );
		nif->set<int>( iSubShapes.child( 0, 0 ), "Vertex Count (?)", vertices.count() );
		nif->setArray<float>( iPackedShape, "Unknown Floats", QVector<float>() << 0.0f << 0.0f << 0.1f << 0.0f << 1.0f << 1.0f << 1.0f << 1.0f << 0.1f );
		nif->set<float>( iPackedShape, "Scale", 1.0f );
		nif->setArray<float>( iPackedShape, "Unknown Floats 2", QVector<float>() << 1.0f << 1.0f << 1.0f );
		
		QModelIndex iPackedData = nif->insertNiBlock( "hkPackedNiTriStripsData", nif->getBlockNumber( iPackedShape ) );
		nif->setLink( iPackedShape, "Data", nif->getBlockNumber( iPackedData ) );
		
		nif->set<int>( iPackedData, "Num Triangles", triangles.count() );
		QModelIndex iTriangles = nif->getIndex( iPackedData, "Triangles" );
		nif->updateArray( iTriangles );
		for ( int t = 0; t < triangles.size(); t++ )
		{
			nif->set<Triangle>( iTriangles.child( t, 0 ), "Triangle", triangles[ t ] );
			nif->set<Vector3>( iTriangles.child( t, 0 ), "Normal", normals.value( t ) );
		}
		
		nif->set<int>( iPackedData, "Num Vertices", vertices.count() );
		QModelIndex iVertices = nif->getIndex( iPackedData, "Vertices" );
		nif->updateArray( iVertices );
		nif->setArray<Vector3>( iVertices, vertices );
		
		QMap<qint32,qint32> lnkmap;
		lnkmap.insert( nif->getBlockNumber( iShape ), nif->getBlockNumber( iPackedShape ) );
		nif->mapLinks( lnkmap );
		
		spRemoveBranch BranchRemover;
		BranchRemover.castIfApplicable( nif, iShape );
		
		return iPackedShape;
	}
};

REGISTER_SPELL( spPackHavokStrips )
