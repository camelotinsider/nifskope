#include "spellbook.h"

#include "spells/blocks.h"
#include "spells/mesh.h"
#include "spells/tangentspace.h"
#include "spells/transform.h"
#include "qtcompat.h"

#include <QBuffer>
#include <QMessageBox>

#include <algorithm> // std::sort
#include <functional> //std::greater

// Brief description is deliberately not autolinked to class Spell
/*! \file optimize.cpp
 * \brief Optimization spells
 *
 * All classes here inherit from the Spell class.
 */

//! Combines properties
/*!
 * This has a tendency to fail due to supposedly boolean values in many NIFs
 * having values apart from 0 and 1.
 *
 * \sa spCombiTris
 * \sa spUniqueProps
 */
class spCombiProps final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Combine Properties" ); }
	QString page() const override final { return Spell::tr( "Optimize" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return nif && !index.isValid();
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & ) override final
	{
		int numRemoved = 0;
		QMap<qint32, QByteArray> props;
		QMap<qint32, qint32> map;

		do {
			props.clear();
			map.clear();

			for ( qint32 b = 0; b < nif->getBlockCount(); b++ ) {
				QModelIndex iBlock = nif->getBlockIndex( b );
				QString original_material_name;

				if ( nif->isNiBlock( iBlock, "NiMaterialProperty" ) ) {
					original_material_name = nif->get<QString>( iBlock, "Name" );

					if ( nif->get<QString>( iBlock, "Name" ).contains( "Material" ) )
						nif->set<QString>( iBlock, "Name", "Material" );
					else if ( nif->get<QString>( iBlock, "Name" ).contains( "Default" ) )
						nif->set<QString>( iBlock, "Name", "Default" );
				}

				if ( nif->blockInherits( iBlock, "BSShaderProperty" ) || nif->isNiBlock( iBlock, "BSShaderTextureSet" ) ) {
					// these need to be unique
					continue;
				}

				if ( nif->blockInherits( iBlock, "NiProperty" ) || nif->blockInherits( iBlock, "NiSourceTexture" ) ) {
					QBuffer data;
					data.open( QBuffer::WriteOnly );
					data.write( nif->itemName( iBlock ).toLatin1() );
					nif->saveIndex( data, iBlock );
					props.insert( b, data.buffer() );
				}

				// restore name
				if ( nif->isNiBlock( iBlock, "NiMaterialProperty" ) ) {
					nif->set<QString>( iBlock, "Name", original_material_name );
				}
			}

			for ( const auto x : props.keys() ) {
				for ( const auto y : props.keys() ) {
					if ( x < y && ( !map.contains( y ) ) && props[x].size() == props[y].size() ) {
						int c = 0;

						while ( c < props[x].size() ) {
							if ( props[x][c] != props[y][c] )
								break;
							c++;
						}

						if ( c == props[x].size() )
							map.insert( y, x );
					}
				}
			}

			if ( !map.isEmpty() ) {
				numRemoved += map.count();
				nif->mapLinks( map );
				QList<qint32> l = map.keys();
				std::sort( l.begin(), l.end(), std::greater<qint32>() );
				for ( const auto b : l ) {
					nif->removeNiBlock( b );
				}
			}
		} while ( !map.isEmpty() );

		Message::info( nullptr, Spell::tr( "Removed %1 properties" ).arg( numRemoved ) );
		return QModelIndex();
	}
};

REGISTER_SPELL( spCombiProps )

//! Creates unique properties from shared ones
/*!
 * \sa spDuplicateBlock
 * \sa spCombiProps
 */
class spUniqueProps final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Split Properties" ); }
	QString page() const override final { return Spell::tr( "Optimize" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return nif && !index.isValid();
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		for ( int b = 0; b < nif->getBlockCount(); b++ ) {
			QModelIndex iAVObj = nif->getBlockIndex( b, "NiAVObject" );

			if ( iAVObj.isValid() ) {
				QVector<qint32> props = nif->getLinkArray( iAVObj, "Properties" );
				QMutableVectorIterator<qint32> it( props );

				while ( it.hasNext() ) {
					qint32 & l = it.next();
					QModelIndex iProp = nif->getBlockIndex( l, "NiProperty" );

					if ( iProp.isValid() && nif->getParent( l ) != b ) {
						QMap<qint32, qint32> map;

						if ( nif->isNiBlock( iProp, "NiTexturingProperty" ) ) {
							for ( const auto sl : nif->getChildLinks( nif->getBlockNumber( iProp ) ) ) {
								QModelIndex iSrc = nif->getBlockIndex( sl, "NiSourceTexture" );

								if ( iSrc.isValid() && !map.contains( sl ) ) {
									QModelIndex iSrc2 = nif->insertNiBlock( "NiSourceTexture", nif->getBlockCount() + 1 );
									QBuffer buffer;
									buffer.open( QBuffer::WriteOnly );
									nif->saveIndex( buffer, iSrc );
									buffer.close();
									buffer.open( QBuffer::ReadOnly );
									nif->loadIndex( buffer, iSrc2 );
									map[ sl ] = nif->getBlockNumber( iSrc2 );
								}
							}
						}

						QModelIndex iProp2 = nif->insertNiBlock( nif->itemName( iProp ), nif->getBlockCount() + 1 );
						QBuffer buffer;
						buffer.open( QBuffer::WriteOnly );
						nif->saveIndex( buffer, iProp );
						buffer.close();
						buffer.open( QBuffer::ReadOnly );
						nif->loadAndMapLinks( buffer, iProp2, map );
						l = nif->getBlockNumber( iProp2 );
					}
				}

				nif->setLinkArray( iAVObj, "Properties", props );
			}
		}

		return index;
	}
};

REGISTER_SPELL( spUniqueProps )

//! Removes nodes with no children and singular parents
/*!
 * Note that the user might lose "important" named nodes with this; short of
 * asking for confirmation or simply reporting nodes instead of removing
 * them, there's not much that can be done to prevent a NIF that won't work
 * ingame.
 */
class spRemoveBogusNodes final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Remove Bogus Nodes" ); }
	QString page() const override final { return Spell::tr( "Optimize" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return nif && !index.isValid();
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		Q_UNUSED( index );
		bool removed;
		int cnt = 0;

		do {
			removed = false;

			for ( int b = 0; b < nif->getBlockCount(); b++ ) {
				QModelIndex iNode = nif->getBlockIndex( b, "NiNode" );

				if ( iNode.isValid() ) {
					if ( nif->getChildLinks( b ).isEmpty() && nif->getParentLinks( b ).isEmpty() ) {
						int x = 0;

						for ( int c = 0; c < nif->getBlockCount(); c++ ) {
							if ( c != b ) {
								if ( nif->getChildLinks( c ).contains( b ) )
									x++;

								if ( nif->getParentLinks( c ).contains( b ) )
									x = 2;

								if ( x >= 2 )
									break;
							}
						}

						if ( x < 2 ) {
							removed = true;
							cnt++;
							nif->removeNiBlock( b );
							break;
						}
					}
				}
			}
		} while ( removed );

		if ( cnt > 0 )
			Message::info( nullptr, Spell::tr( "Removed %1 nodes" ).arg( cnt ) );

		return QModelIndex();
	}
};

REGISTER_SPELL( spRemoveBogusNodes )

//! Combines geometry data
/*!
 * Can fail for a number of reasons, usually due to mismatched properties (see
 * spCombiProps for why that can fail) or non-geometry children (extra data,
 * skin instance etc.).
 */
class spCombiTris final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Combine Shapes" ); }
	QString page() const override final { return Spell::tr( "Optimize" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return nif && nif->isNiBlock( index, "NiNode" );
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		// join meshes which share properties and parent
		// ( animated ones are left untouched )

		QPersistentModelIndex iParent( index );

		// populate a list of possible candidates

		QList<qint32> lTris;

		for ( const auto lChild : nif->getLinkArray( iParent, "Children" ) ) {
			if ( nif->getParent( lChild ) == nif->getBlockNumber( iParent ) ) {
				QModelIndex iChild = nif->getBlockIndex( lChild );

				if ( nif->isNiBlock( iChild, { "NiTriShape", "NiTriStrips" } ) )
					lTris << lChild;
			}
		}

		// detect matches

		QMap<qint32, QList<qint32> > match;
		QList<qint32> found;

		for ( const auto lTriA : lTris ) {
			if ( found.contains( lTriA ) )
				continue;

			for ( const auto lTriB : lTris ) {
				if ( matches( nif, nif->getBlockIndex( lTriA ), nif->getBlockIndex( lTriB ) ) ) {
					match[ lTriA ] << lTriB;
					found << lTriB;
				}
			}
		}

		// combine the matches

		spApplyTransformation ApplyTransform;
		spTangentSpace TSpace;

		QList<QPersistentModelIndex> remove;

		for ( const auto lTriA : match.keys() ) {
			ApplyTransform.cast( nif, nif->getBlockIndex( lTriA ) );

			for ( const auto lTriB : match[lTriA] ) {
				ApplyTransform.cast( nif, nif->getBlockIndex( lTriB ) );
				combine( nif, nif->getBlockIndex( lTriA ), nif->getBlockIndex( lTriB ) );
				remove << nif->getBlockIndex( lTriB );
			}

			TSpace.castIfApplicable( nif, nif->getBlockIndex( lTriA ) );
		}

		// remove the now obsolete shapes

		spRemoveBranch BranchRemover;

		for ( const QPersistentModelIndex& rem : remove ) {
			BranchRemover.cast( nif, rem );
		}

		return iParent;
	}

	//! Determine if two shapes are identical
	bool matches( const NifModel * nif, const QModelIndex & iTriA, const QModelIndex & iTriB )
	{
		if ( iTriA == iTriB
			 || nif->itemName( iTriA ) != nif->itemName( iTriB )
		     || nif->get<int>( iTriA, "Flags" ) != nif->get<int>( iTriB, "Flags" ) )
		{
			return false;
		}

		QVector<qint32> lPrpsA = nif->getLinkArray( iTriA, "Properties" );
		QVector<qint32> lPrpsB = nif->getLinkArray( iTriB, "Properties" );

		std::sort( lPrpsA.begin(), lPrpsA.end() /*, std::less<qint32>()*/ );
		std::sort( lPrpsB.begin(), lPrpsB.end() /*, std::less<qint32>()*/ );

		if ( lPrpsA != lPrpsB )
			return false;

		for ( const auto l : nif->getChildLinks( nif->getBlockNumber( iTriA ) ) ) {
			if ( lPrpsA.contains( l ) )
				continue;

			QModelIndex iBlock = nif->getBlockIndex( l );

			if ( nif->isNiBlock( iBlock, "NiTriShapeData" ) )
				continue;
			if ( nif->isNiBlock( iBlock, "NiTriStripsData" ) )
				continue;
			if ( nif->isNiBlock( iBlock, "NiBinaryExtraData" ) && nif->get<QString>( iBlock, "Name" ) == "Tangent space (binormal & tangent vectors)" )
				continue;

			qCWarning( nsSpell ) << Spell::tr( "Attached %1 prevents %2 and %3 from matching." )
				.arg( nif->itemName( iBlock ) )
				.arg( nif->get<QString>( iTriA, "Name" ) )
				.arg( nif->get<QString>( iTriB, "Name" ) );

			return false;
		}

		for ( const auto l : nif->getChildLinks( nif->getBlockNumber( iTriB ) ) ) {
			if ( lPrpsB.contains( l ) )
				continue;

			QModelIndex iBlock = nif->getBlockIndex( l );

			if ( nif->isNiBlock( iBlock, "NiTriShapeData" ) )
				continue;
			if ( nif->isNiBlock( iBlock, "NiTriStripsData" ) )
				continue;
			if ( nif->isNiBlock( iBlock, "NiBinaryExtraData" ) && nif->get<QString>( iBlock, "Name" ) == "Tangent space (binormal & tangent vectors)" )
				continue;

			qCWarning( nsSpell ) << Spell::tr( "Attached %1 prevents %2 and %3 from matching." )
				.arg( nif->itemName( iBlock ) )
				.arg( nif->get<QString>( iTriA, "Name" ) )
				.arg( nif->get<QString>( iTriB, "Name" ) );

			return false;
		}

		QModelIndex iDataA = nif->getBlockIndex( nif->getLink( iTriA, "Data" ), "NiTriBasedGeomData" );
		QModelIndex iDataB = nif->getBlockIndex( nif->getLink( iTriB, "Data" ), "NiTriBasedGeomData" );

		return dataMatches( nif, iDataA, iDataB );
	}

	//! Determines if two sets of shape data are identical
	bool dataMatches( const NifModel * nif, const QModelIndex & iDataA, const QModelIndex & iDataB )
	{
		if ( iDataA == iDataB )
			return true;

		for ( const QString& id : QStringList{ "Vertices", "Normals", "Vertex Colors", "UV Sets" } ) {
			QModelIndex iA = nif->getIndex( iDataA, id );
			QModelIndex iB = nif->getIndex( iDataB, id );

			if ( iA.isValid() != iB.isValid() )
				return false;

			if ( id == "UV Sets" && nif->rowCount( iA ) != nif->rowCount( iB ) )
				return false;
		}
		return true;
	}

	//! Combines meshes a and b ( a += b )
	void combine( NifModel * nif, const QModelIndex & iTriA, const QModelIndex & iTriB )
	{
		nif->set<quint32>( iTriB, "Flags", nif->get<quint32>( iTriB, "Flags" ) | 1 );

		QModelIndex iDataA = nif->getBlockIndex( nif->getLink( iTriA, "Data" ), "NiTriBasedGeomData" );
		QModelIndex iDataB = nif->getBlockIndex( nif->getLink( iTriB, "Data" ), "NiTriBasedGeomData" );

		int numA = nif->get<int>( iDataA, "Num Vertices" );
		int numB = nif->get<int>( iDataB, "Num Vertices" );
		nif->set<int>( iDataA, "Num Vertices", numA + numB );

		nif->updateArraySize( iDataA, "Vertices" );
		nif->setArray<Vector3>( iDataA, "Vertices", nif->getArray<Vector3>( iDataA, "Vertices" ).mid( 0, numA ) + nif->getArray<Vector3>( iDataB, "Vertices" ) );

		nif->updateArraySize( iDataA, "Normals" );
		nif->setArray<Vector3>( iDataA, "Normals", nif->getArray<Vector3>( iDataA, "Normals" ).mid( 0, numA ) + nif->getArray<Vector3>( iDataB, "Normals" ) );

		nif->updateArraySize( iDataA, "Vertex Colors" );
		nif->setArray<Color4>( iDataA, "Vertex Colors", nif->getArray<Color4>( iDataA, "Vertex Colors" ).mid( 0, numA ) + nif->getArray<Color4>( iDataB, "Vertex Colors" ) );

		QModelIndex iUVa = nif->getIndex( iDataA, "UV Sets" );
		QModelIndex iUVb = nif->getIndex( iDataB, "UV Sets" );

		for ( int r = 0; r < nif->rowCount( iUVa ); r++ ) {
			nif->updateArraySize( QModelIndex_child( iUVa, r ) );
			nif->setArray<Vector2>( QModelIndex_child( iUVa, r ), nif->getArray<Vector2>( QModelIndex_child( iUVa, r ) ).mid( 0, numA ) + nif->getArray<Vector2>( QModelIndex_child( iUVb, r ) ) );
		}

		int triCntA = nif->get<int>( iDataA, "Num Triangles" );
		int triCntB = nif->get<int>( iDataB, "Num Triangles" );
		nif->set<int>( iDataA, "Num Triangles", triCntA + triCntB );
		nif->set<int>( iDataA, "Num Triangle Points", ( triCntA + triCntB ) * 3 );

		QVector<Triangle> triangles = nif->getArray<Triangle>( iDataB, "Triangles" );
		QMutableVectorIterator<Triangle> itTri( triangles );

		while ( itTri.hasNext() ) {
			Triangle & tri = itTri.next();
			tri[0] += numA;
			tri[1] += numA;
			tri[2] += numA;
		}

		nif->updateArraySize( iDataA, "Triangles" );
		nif->setArray<Triangle>( iDataA, "Triangles", triangles + nif->getArray<Triangle>( iDataA, "Triangles" ) );

		int stripCntA = nif->get<int>( iDataA, "Num Strips" );
		int stripCntB = nif->get<int>( iDataB, "Num Strips" );
		nif->set<int>( iDataA, "Num Strips", stripCntA + stripCntB );

		nif->updateArraySize( iDataA, "Strip Lengths" );
		nif->updateArraySize( iDataA, "Points" );

		for ( int r = 0; r < stripCntB; r++ ) {
			QVector<quint16> strip = nif->getArray<quint16>( QModelIndex_child( nif->getIndex( iDataB, "Points" ), r ) );
			QMutableVectorIterator<quint16> it( strip );

			while ( it.hasNext() )
				it.next() += numA;

			nif->set<int>( QModelIndex_child( nif->getIndex( iDataA, "Strip Lengths" ), r + stripCntA ), strip.size() );
			nif->updateArraySize( QModelIndex_child( nif->getIndex( iDataA, "Points" ), r + stripCntA ) );
			nif->setArray<quint16>( QModelIndex_child( nif->getIndex( iDataA, "Points" ), r + stripCntA ), strip );
		}

		spUpdateCenterRadius CenterRadius;
		CenterRadius.castIfApplicable( nif, iDataA );
	}
};

REGISTER_SPELL( spCombiTris )


void scan( const QModelIndex & idx, NifModel * nif, QMap<QString, qint32> & usedStrings, bool hasCED )
{
	for ( int i = 0; i < nif->rowCount( idx ); i++ ) {
		auto child = QModelIndex_child( idx, i, 2 );
		if ( nif->rowCount( child ) > 0 ) {
			scan( child, nif, usedStrings, hasCED );
			continue;
		}

		NifValue val;
		val.setFromVariant( child.data( Qt::EditRole ) );
		if ( val.type() == NifValue::tStringIndex ) {
			if ( nif->get<int>( child ) == -1 )
				continue;

			auto str = nif->get<QString>( child );
			if ( !usedStrings.contains( str ) )
				usedStrings.insert( str, usedStrings.size() );

			qint32 value = usedStrings[str];
			if ( hasCED && value > 0 )
				value++;

			nif->set<int>( child, value );
		}
	}
}

//! Removes unused strings from the header
class spRemoveUnusedStrings final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Remove Unused Strings" ); }
	QString page() const override final { return Spell::tr( "Optimize" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return nif && !index.isValid();
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & ) override final
	{
		auto originalStrings = nif->getArray<QString>( nif->getHeaderIndex(), "Strings" );

		// FO4 workaround for apparently unused but necessary BSClothExtraData string
		int cedIdx = originalStrings.indexOf( "CED" );

		bool hasCED = cedIdx >= 0;

		QMap<QString, qint32> usedStrings;
		for ( qint32 b = 0; b < nif->getBlockCount(); b++ )
			scan( nif->getBlockIndex( b ), nif, usedStrings, hasCED );

		QVector<QString> newStrings( usedStrings.size() );
		for ( auto kv : usedStrings.toStdMap() )
			newStrings[kv.second] = kv.first;

		int newSize = newStrings.size();

		if ( hasCED ) {
			newStrings.insert( cedIdx, 1, "CED" );
			newSize++;
		}

		nif->set<uint>( nif->getHeaderIndex(), "Num Strings", newSize );
		nif->updateArraySize( nif->getHeaderIndex(), "Strings" );
		nif->setArray<QString>( nif->getHeaderIndex(), "Strings", newStrings );
		nif->updateHeader();

		// Remove new from original to see what was removed
		for ( const auto & s : newStrings )
			originalStrings.removeAll( s );

		QString msg;
		if ( originalStrings.size() )
			msg = "Removed:\r\n" + QStringList::fromVector( originalStrings ).join( "\r\n" );

		Message::info( nullptr, Spell::tr( "Strings Removed: %1. New string table has %2 entries." )
					   .arg( originalStrings.size() ).arg( newSize ), msg
		);

		return QModelIndex();
	}
};

REGISTER_SPELL( spRemoveUnusedStrings )
